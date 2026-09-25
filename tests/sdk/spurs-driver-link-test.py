#!/usr/bin/env python3
"""Link and ownership controls for a private SPURS driver candidate."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
from spurs_guid_check import inspect as inspect_guid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ps3dev', type=Path, default=os.environ.get('PS3DEV'))
p.add_argument('--prefix', type=Path, help='Private startup/service install prefix')
p.add_argument('--specs', type=Path)
p.add_argument('--output', type=Path, default=Path('build/spurs-driver-links'))
a = p.parse_args()
if not a.ps3dev:
    print('SPURS driver links: SKIP (set PS3DEV or --ps3dev)')
    raise SystemExit(0)
a.prefix = a.prefix or a.ps3dev
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
lib = a.prefix.resolve()/'spu/spu-elf/lib'
sdk = a.ps3dev.resolve()/'ps3dk'
rows = []


def run(name, cmd, failure=None):
    r = subprocess.run([str(x) for x in cmd], capture_output=True, text=True)
    (out/(name+'.log')).write_text(r.stdout+r.stderr)
    ok = r.returncode != 0 and failure in r.stderr if failure else r.returncode == 0
    row = dict(name=name, rc=r.returncode, ok=ok, command=[str(x) for x in cmd])
    rows.append(row)
    return row


cases = [('task', 'canonical'), ('task', 'legacy'), ('task', 'both'), ('task', 'missing'),
         ('job', 'direct'), ('job', 'missing'), ('initialized', 'direct'),
         ('initialized', 'queue'), ('initialized', 'both'), ('initialized', 'missing')]
for lang, driver in [('c', 'gcc'), ('c++', 'g++')]:
    cc = a.ps3dev.resolve()/('spu/bin/spu-elf-'+driver)
    common = [cc, '-B'+str(lib)+'/', '-isystem', root/'sdk/libspurs_task/include',
              '-isystem', root/'sdk/libspurs_job/include', '-isystem', sdk/'spu/include']
    if a.specs:
        common += ['-specs='+str(a.specs.resolve())]
    for mode, case in cases:
        name = lang+'-'+mode+'-'+case
        stem = out/name
        src, obj, elf = stem.with_suffix('.c'), stem.with_suffix('.o'), stem.with_suffix('.elf')
        body = '#include <cell/spurs/task.h>\n' if mode=='task' else '#include <cell/spurs/job_chain.h>\n#include <cell/spurs/job_queue.h>\n'
        if mode=='task':
            if case in ('canonical', 'both'):
                body += 'int cellSpursTaskMain(qword a, uint64_t b) { (void)a; (void)b; return 37; }\n'
            if case in ('legacy', 'both'):
                body += 'void cellSpursMain(qword a, uint64_t b) { (void)a; (void)b; }\n'
        else:
            if case in ('direct', 'both'):
                body += 'void cellSpursJobMain2(CellSpursJobContext2 *a, CellSpursJob256 *b) { (void)a; (void)b; }\n'
            if case in ('queue', 'both'):
                body += 'void cellSpursJobQueueMain(CellSpursJobContext2 *a, CellSpursJob256 *b) { (void)a; (void)b; }\n'
        src.write_text(body)
        pic = [] if mode=='task' else ['-fpic']
        if not run(name+'-compile', common+pic+['-x', lang, '-O2', '-c', src, '-o', obj])['ok']:
            continue
        flag = '-mspurs-'+('job-initialize' if mode=='initialized' else mode)
        expected = ('cellSpursTaskMain' if mode=='task' else 'cellSpursJobQueueMain' if mode=='initialized' else 'cellSpursJobMain2') if case=='missing' else None
        cmd = common+pic+[flag, '-Wl,-q', obj, '-Wl,-Map,'+str(stem.with_suffix('.map'))+',--cref', '-o', elf]
        row = run(name+'-link', cmd, expected)
        if not row['ok'] or expected:
            continue
        data = elf.read_bytes()
        row['entry'], row['flags'] = struct.unpack_from('>I', data, 24)[0], struct.unpack_from('>I', data, 36)[0]
        row['ok'] &= data[:6]==b'\x7fELF\x01\x02' and row['entry']==(0x3000 if mode=='task' else 0x10)
        row['ok'] &= row['flags']=={'task':3,'job':1,'initialized':2}[mode]
        if mode != 'task':
            try:
                row['guid'] = inspect_guid(elf)
            except ValueError as error:
                row['guid_error'] = str(error)
                row['ok'] = False
        text = stem.with_suffix('.map').read_text()
        table = text.split('Cross Reference Table',1)[1].splitlines()
        def owner(sym):
            return next((l.split(None,1)[1].strip() for l in table if l.startswith(sym+' ')), 'MISSING')
        entry = '__spurs_task_start' if mode=='task' else '_start'
        row['owners'] = {s:owner(s) for s in [entry, 'cellSpursMain', 'cellSpursJobMain2']}
        expected_start = {'task':'spurs_task.o', 'job':'job_start.o', 'initialized':'job_start_w_crt.o'}[mode]
        row['ok'] &= owner(entry)==str(lib/expected_start)
        if mode=='task':
            row['ok'] &= owner('cellSpursMain')==(str(lib/'libspurs_task_runtime.a')+'(spurs_task_main.o)' if case=='canonical' else str(obj))
        else:
            row['ok'] &= owner('cellSpursJobMain2')==(str(lib/'libspurs_jq_runtime.a')+'(job_queue_main.o)' if case=='queue' else str(obj))
        if mode=='task' and case=='canonical':
            # User scripts must win whether passed to gcc or forwarded to ld.
            custom = out/(lang+'-custom-task.ld')
            custom.write_text((root/'sdk/libspurs_task/scripts/spurs_task.ld').read_text().replace('0x3000', '0x5000'))
            for kind, option in [('T', ['-T', custom]), ('Wl-T', ['-Wl,-T,'+str(custom)])]:
                target = out/(lang+'-custom-'+kind+'.elf')
                rr = run(lang+'-custom-'+kind, common+[flag, obj, *option, '-o', target])
                if rr['ok']:
                    rr['entry'] = struct.unpack_from('>I', target.read_bytes(),24)[0]
                    rr['ok'] &= rr['entry']==0x5000
            target = out/(lang+'-nostartfiles.elf')
            rr = run(lang+'-nostartfiles', common+[flag, '-nostartfiles', obj, '-o', target])
            if rr['ok']:
                nm = subprocess.run([str(a.ps3dev.resolve()/'spu/bin/spu-elf-nm'), '--defined-only', str(target)], capture_output=True, text=True)
                (out/(lang+'-nostartfiles.nm')).write_text(nm.stdout+nm.stderr)
                rr['ok'] &= nm.returncode==0 and '__spurs_task_start' not in nm.stdout
            for option in ['-nodefaultlibs', '-nostdlib']:
                # Trace controls prove exact selection; nodefaultlibs must leave
                # an unresolved CRT call instead of silently injecting services.
                if option=='-nodefaultlibs':
                    run(lang+'-'+option[1:], common+[flag, option, obj, '-o', out/(lang+option+'.elf')], 'cellSpursMain')

    ordinary = out/(lang+'-ordinary.c')
    ordinary.write_text('int main(void) { return 0; }\n')
    run(lang+'-ordinary-link', common+['-x',lang,ordinary,'-o',out/(lang+'-ordinary.elf')])

for row in rows:
    print(row['name'], 'PASS' if row['ok'] else 'FAIL')
(out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
raise SystemExit(0 if all(r['ok'] for r in rows) else 1)
