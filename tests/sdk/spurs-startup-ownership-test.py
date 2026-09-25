#!/usr/bin/env python3
"""Validate independently selectable SPURS startup and service archives."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--libdir', type=Path)
p.add_argument('--nm')
p.add_argument('--output', type=Path, default=Path('build/spurs-startup-ownership'))
a = p.parse_args()
dev = os.environ.get('PS3DEV')
if not a.libdir and not dev:
    print('SPURS startup ownership: SKIP (set PS3DEV or --libdir/--nm)')
    raise SystemExit(0)
a.libdir = a.libdir or Path(dev)/'spu/spu-elf/lib'
a.nm = a.nm or (str(Path(dev)/'spu/bin/spu-elf-nm') if dev else 'spu-elf-nm')
a.output.mkdir(parents=True, exist_ok=True)
rows = []
configs = [('task', ['spurs_task.o'], '__spurs_task_start'),
           ('job', ['job_start.o'], '_start'),
           ('jq', ['job_start_w_crt.o', 'job_crt.o'], '_start')]
for mode, starts, entry in configs:
    full = a.libdir / ('libspurs_'+mode+'.a')
    service = a.libdir / ('libspurs_'+mode+'_runtime.a')
    files = [full, service] + [a.libdir/x for x in starts]
    row = {'mode':mode, 'ok':False, 'missing':[str(x) for x in files if not x.exists()]}
    rows.append(row)
    if row['missing']:
        continue
    symbols = {}
    for f in files:
        r = subprocess.run([a.nm, '-A', '--defined-only', str(f)], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr)
        symbols[f.name] = r.stdout
        (a.output/(f.name+'.nm')).write_text(r.stdout)
    has = lambda file, sym: any(line.split()[-1:] == [sym] for line in symbols[file].splitlines())
    row['ok'] = has(full.name, entry) and not has(service.name, entry) and has(starts[0], entry)
    row['ok'] &= not any(has(service.name, s) for s in ['_start', '__spurs_task_start', '__job_start'])
    if mode=='jq':
        defs = [line for line in symbols[service.name].splitlines() if line.split()[-1:] == ['cellSpursJobMain2']]
        row['ok'] &= (len(defs)==1 and 'job_queue_main.o:' in defs[0]
                      and defs[0].split()[-2] == 'T')
        row['ok'] &= has('job_crt.o', '__job_start')
for row in rows:
    print(row['mode'], 'PASS' if row['ok'] else 'FAIL', row['missing'])
(a.output/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
raise SystemExit(0 if all(r['ok'] for r in rows) else 1)
