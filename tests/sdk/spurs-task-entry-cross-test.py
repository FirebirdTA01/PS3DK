#!/usr/bin/env python3
"""SPU task entry declaration, archive extraction and link-layout controls."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--ps3dev', type=Path, default=os.environ.get('PS3DEV'))
p.add_argument('--runtime', type=Path, help='Candidate libspurs_task.a')
p.add_argument('--headers', type=Path, help='Candidate include directory')
p.add_argument('--output', type=Path, default=ROOT/'build/spurs-task-entry')
args = p.parse_args()
if not args.ps3dev:
    print('SPURS task entry cross: SKIP (PS3DEV not set)')
    raise SystemExit(0)
dev = args.ps3dev.resolve()
sdk = dev/'ps3dk'
headers = args.headers.resolve() if args.headers else sdk/'spu/include'
runtime = args.runtime.resolve() if args.runtime else sdk/'spu/lib/libspurs_task.a'
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
rows = []


def run(name, cmd, expect_failure=None):
    r = subprocess.run(cmd, capture_output=True, text=True)
    (out/(name+'.log')).write_text(r.stdout+r.stderr)
    ok = r.returncode != 0 and expect_failure in r.stderr if expect_failure else r.returncode == 0
    row = dict(name=name, command=cmd, rc=r.returncode, ok=ok)
    rows.append(row)
    return row


for lang, compiler in [('c', 'gcc'), ('c++', 'g++')]:
    cc = str(dev/('spu/bin/spu-elf-'+compiler))
    probe = out/(lang+'-declaration.c')
    probe.write_text('#include <cell/spurs/task.h>\nint (*entry)(qword,uint64_t) = cellSpursTaskMain;\n')
    run(lang+'-declaration', [cc, '-x', lang, '-I'+str(headers), '-Werror', '-c', str(probe), '-o', str(out/(lang+'-declaration.o'))])
    for case in ['canonical', 'legacy', 'both', 'missing']:
        stem = out/(lang+'-'+case)
        source = stem.with_suffix('.c')
        text = '#include <cell/spurs/task.h>\n'
        if case in ['canonical', 'both']:
            text += 'int cellSpursTaskMain(qword a, uint64_t b) { (void)a; (void)b; return 37; }\n'
        if case in ['legacy', 'both']:
            text += 'void cellSpursMain(qword a, uint64_t b) { (void)a; (void)b; }\n'
        source.write_text(text)
        obj = stem.with_suffix('.o')
        compiled = run(lang+'-'+case+'-compile', [cc, '-x', lang, '-I'+str(headers), '-O2', '-c', str(source), '-o', str(obj)])
        if not compiled['ok']:
            continue
        elf = stem.with_suffix('.elf')
        row = run(lang+'-'+case+'-link', [cc, '-nostartfiles', '-mspurs-task',
                  '-T'+str(ROOT/'sdk/libspurs_task/scripts/spurs_task.ld'), str(obj),
                  str(runtime), '-Wl,-Map,'+str(stem.with_suffix('.map'))+',--cref', '-o', str(elf)],
                  'cellSpursTaskMain' if case == 'missing' else None)
        if row['ok'] and case != 'missing':
            data = elf.read_bytes()
            crossref = stem.with_suffix('.map').read_text().split('Cross Reference Table', 1)[1].splitlines()
            def owner(symbol):
                return next((line.split(None, 1)[1].strip() for line in crossref if line.startswith(symbol+' ')), 'MISSING')
            row['owners'] = {s:owner(s) for s in ['cellSpursMain', 'cellSpursTaskMain', '__spurs_task_start']}
            expected = str(runtime)+'(spurs_task_main.o)' if case == 'canonical' else str(obj)
            row['ok'] = (data[:6] == b'\x7fELF\x01\x02' and
                         struct.unpack_from('>I', data, 24)[0] == 0x3000 and
                         struct.unpack_from('>I', data, 36)[0] == 3 and
                         owner('cellSpursMain') == expected and
                         owner('__spurs_task_start') == str(runtime)+'(crt_spurs_task.o)' and
                         (case == 'legacy' or owner('cellSpursTaskMain') == str(obj)))
            row['entry'] = struct.unpack_from('>I', data, 24)[0]
            row['flags'] = struct.unpack_from('>I', data, 36)[0]
for row in rows:
    print(row['name'], 'PASS' if row['ok'] else 'FAIL', flush=True)
(out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
raise SystemExit(0 if all(row['ok'] for row in rows) else 1)
