#!/usr/bin/env python3
"""Check canonical SPU service ownership independently of startup selection."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

from spurs_guid_check import inspect as inspect_guid

MEMBERS = {
    'spurs_task_exit.o', 'spurs_task_runtime.o', 'spurs_module_runtime.o',
    'spurs_can_call_block_wait.o', 'spurs_get_workload_flag.o',
    'spurs_send_workload_signal.o', 'spurs_send_signal.o',
    'spurs_semaphore_p.o', 'spurs_semaphore_v.o',
}
FORBIDDEN = {'_start', '__spurs_task_start', '__job_start', 'cellSpursMain',
             'cellSpursTaskMain', 'cellSpursJobMain2', 'cellSpursJobQueueMain'}
EXPORTS = {
    'cellSpursExit', 'cellSpursTaskExit', 'cellSpursGetTaskId',
    'cellSpursGetTasksetAddress', 'cellSpursTaskPoll', 'cellSpursYield',
    '_cellSpursGetIWLTaskId', 'cellSpursGetCurrentSpuId', 'cellSpursGetElfAddress',
    'cellSpursGetSpuCount', 'cellSpursGetSpursAddress', 'cellSpursGetTagId',
    'cellSpursGetWorkloadId', 'cellSpursModuleExit', 'cellSpursModulePoll',
    'cellSpursModulePollStatus', 'cellSpursPoll', '_cellSpursTaskCanCallBlockWait',
    '_cellSpursGetWorkloadFlag', '_cellSpursSendWorkloadSignal',
    'cellSpursSendSignal', 'cellSpursSemaphoreP', 'cellSpursSemaphoreV',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps3dev', type=Path, default=os.environ.get('PS3DEV'))
    parser.add_argument('--output', type=Path, default=Path('build/spurs-common'))
    args = parser.parse_args()
    if not args.ps3dev:
        print('SPURS common archive: SKIP (set PS3DEV or --ps3dev)')
        return 0
    dev, out = args.ps3dev.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lib = dev/'spu/spu-elf/lib'
    archive = lib/'libspurs.a'
    rows = []

    def run(name, cmd, failure=None):
        result = subprocess.run([str(x) for x in cmd], capture_output=True, text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        ok = (result.returncode == 1 and failure in result.stderr if failure
              else result.returncode == 0)
        row = dict(name=name, rc=result.returncode, ok=ok, command=list(map(str, cmd)))
        rows.append(row)
        return row, result.stdout

    def inventory(path):
        members = subprocess.run([str(dev/'spu/bin/spu-elf-ar'), 't', str(path)],
                                 capture_output=True, text=True)
        nm = subprocess.run([str(dev/'spu/bin/spu-elf-nm'), '-A', '-g', '--defined-only', str(path)],
                            capture_output=True, text=True)
        definitions = [line.split() for line in nm.stdout.splitlines() if len(line.split()) >= 3]
        symbols = {line[-1] for line in definitions}
        names = members.stdout.splitlines()
        return (members.returncode == nm.returncode == 0 and len(names) == len(MEMBERS)
                and set(names) == MEMBERS and not symbols.intersection(FORBIDDEN)
                and symbols == EXPORTS and len(definitions) == len(EXPORTS)
                and all(line[-2]=='T' for line in definitions)), nm.stdout

    if not archive.is_file():
        rows.append(dict(name='canonical-archive', ok=False, missing=str(archive)))
    else:
        ok, symbols = inventory(archive)
        (out/'common.nm').write_text(symbols)
        rows.append(dict(name='canonical-archive', ok=ok))
        mirror = dev/'ps3dk/spu/lib/libspurs.a'
        rows.append(dict(name='install-mirror', ok=mirror.is_file() and mirror.read_bytes()==archive.read_bytes()))
    if not all(row['ok'] for row in rows):
        (out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
        print('SPURS common archive: FAIL (missing or invalid service archive)')
        return 1

    # A real archive contaminated with an existing startup object must fail the
    # same inventory check used above, even if ordinary lazy links would pass.
    mutant = out/'mutant.a'
    shutil.copy2(archive, mutant)
    row, _ = run('mutant-insert', [dev/'spu/bin/spu-elf-ar', 'r', mutant, lib/'spurs_task.o'])
    row['ok'] &= not inventory(mutant)[0]
    # The entry adapter is also forbidden even though it is not a CRT object.
    adapter = out/'spurs_task_main.o'
    extraction = subprocess.run([str(dev/'spu/bin/spu-elf-ar'), 'p',
                                  str(lib/'libspurs_task_runtime.a'), adapter.name], capture_output=True)
    if extraction.returncode or not extraction.stdout:
        raise RuntimeError('cannot extract real entry adapter mutant')
    adapter.write_bytes(extraction.stdout)
    mutant = out/'adapter-mutant.a'
    shutil.copy2(archive, mutant)
    row, _ = run('adapter-mutant-insert', [dev/'spu/bin/spu-elf-ar', 'r', mutant, adapter])
    row['ok'] &= not inventory(mutant)[0]

    for language, driver in [('c', 'gcc'), ('c++', 'g++')]:
        cc = dev/('spu/bin/spu-elf-'+driver)
        base = [cc, '-isystem', dev/'ps3dk/spu/include']
        for mode in ['ordinary', 'task', 'job', 'initialized']:
            name = language+'-'+mode
            src, obj = out/(name+'.c'), out/(name+'.o')
            body = ('#include <cell/spurs/common.h>\n#include <cell/spurs/semaphore.h>\n'
                    '#include <cell/spurs/task.h>\n#include <cell/spurs/job_chain.h>\n'
                    '#include <cell/spurs/job_queue.h>\n'
                    'volatile unsigned long long observed;\n'
                    'static void use_services(void) { observed = cellSpursGetSpursAddress(); '
                    'observed += cellSpursSemaphoreV(128); }\n')
            entry = {
                'ordinary': 'int main(void) { use_services(); return 0; }',
                'task': 'int cellSpursTaskMain(qword a, uint64_t b) { (void)a; (void)b; use_services(); return 37; }',
                'job': 'void cellSpursJobMain2(CellSpursJobContext2 *a, CellSpursJob256 *b) { (void)a; (void)b; use_services(); }',
                'initialized': 'void cellSpursJobQueueMain(CellSpursJobContext2 *a, CellSpursJob256 *b) { (void)a; (void)b; use_services(); }',
            }[mode]
            src.write_text(body+entry+'\n')
            pic = ['-fpic'] if mode in ['job', 'initialized'] else []
            row, _ = run(name+'-compile', base+pic+['-x', language, '-O2', '-c', src, '-o', obj])
            if not row['ok']:
                continue
            flag = [] if mode == 'ordinary' else ['-mspurs-'+('job-initialize' if mode == 'initialized' else mode)]
            orders = [('canonical', ['-lspurs'])]
            if mode == 'task':
                orders += [('runtime-first', ['-lspurs_task_runtime', '-lspurs']),
                           ('common-first', ['-lspurs', '-lspurs_task_runtime'])]
            for order, libs in orders:
                stem = out/(name+'-'+order)
                elf, mapfile = Path(str(stem)+'.elf'), Path(str(stem)+'.map')
                row, _ = run(name+'-'+order, base+pic+flag+[obj, *libs, '-Wl,-q,-Map,'+str(mapfile)+',--cref', '-o', elf])
                if not row['ok']:
                    continue
                data = elf.read_bytes()
                entry_address, flags = struct.unpack_from('>I', data, 24)[0], struct.unpack_from('>I', data, 36)[0]
                row['ok'] &= data[:7] == b'\x7fELF\x01\x02\x01' and struct.unpack_from('>H',data,18)[0] == 23
                table = mapfile.read_text().split('Cross Reference Table', 1)[1].splitlines()
                def owner(symbol):
                    value = next((line.split(None, 1)[1].strip() for line in table if line.startswith(symbol+' ')), 'MISSING')
                    if value == 'MISSING':
                        return value
                    path, separator, member = value.partition('(')
                    return str(Path(path).resolve()) + (separator+member if separator else '')
                service_archive = 'libspurs_task_runtime.a' if order=='runtime-first' else 'libspurs.a'
                expected = str(lib/service_archive)
                row['owners'] = {s:owner(s) for s in ['cellSpursGetSpursAddress', 'cellSpursSemaphoreV', '_cellSpursSendWorkloadSignal']}
                for symbol, member in [('cellSpursGetSpursAddress','spurs_module_runtime.o'),
                                       ('cellSpursSemaphoreV','spurs_semaphore_v.o'),
                                       ('_cellSpursSendWorkloadSignal','spurs_send_workload_signal.o')]:
                    row['ok'] &= owner(symbol) == expected+'('+member+')'
                row.update(entry=entry_address, flags=flags)
                row['ok'] &= flags == {'ordinary':0,'task':3,'job':1,'initialized':2}[mode]
                start = '__spurs_task_start' if mode=='task' else '_start'
                start_owner = owner(start)
                row['start_owner'] = start_owner
                row['ok'] &= 'libspurs.a(' not in start_owner
                if mode != 'ordinary':
                    row['ok'] &= entry_address == (0x3000 if mode=='task' else 0x10)
                    startup = {'task':'spurs_task.o','job':'job_start.o','initialized':'job_start_w_crt.o'}[mode]
                    row['ok'] &= start_owner == str(lib/startup)
                    if mode != 'task':
                        try:
                            row['guid'] = inspect_guid(elf)
                        except ValueError as error:
                            row.update(ok=False, guid_error=str(error))
                else:
                    row['ok'] &= start_owner.endswith('/crt1.o')
            # No canonical archive is allowed to manufacture the user entry.
            missing = out/(name+'-missing.c')
            missing.write_text('int unrelated_symbol;\n')
            expected = {'ordinary':'main','task':'cellSpursTaskMain','job':'cellSpursJobMain2','initialized':'cellSpursJobQueueMain'}[mode]
            run(name+'-missing-entry', base+pic+flag+['-x',language,missing,'-x','none','-lspurs','-o',out/(name+'-missing.elf')], expected)
    (out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
    for row in rows:
        print(row['name'], 'PASS' if row['ok'] else 'FAIL')
    return 0 if all(row['ok'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
