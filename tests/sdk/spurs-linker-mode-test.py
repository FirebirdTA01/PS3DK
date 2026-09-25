#!/usr/bin/env python3
"""Reject incompatible SPURS modes at ld, including driver-forwarded options."""
import argparse
import itertools
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ld')
p.add_argument('--as', dest='assembler')
p.add_argument('--stock-ld', help='Optional upstream linker for ordinary -r byte comparison')
p.add_argument('--output', type=Path, default=Path('build/spurs-linker-modes'))
a = p.parse_args()
dev = os.environ.get('PS3DEV')
if not a.ld and not dev:
    print('SPURS linker modes: SKIP (set PS3DEV or --ld and --as)')
    raise SystemExit(0)
a.ld = a.ld or str(Path(dev)/'spu/bin/spu-elf-ld')
a.assembler = a.assembler or (str(Path(dev)/'spu/bin/spu-elf-as') if dev else None)
if not a.assembler:
    p.error('--as is required without PS3DEV')
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source, obj = out/'fixture.s', out/'fixture.o'
source.write_text('.section .text\n.global _start\n_start:\n nop\n')
assembled = subprocess.run([a.assembler, str(source), '-o', str(obj)],
                           capture_output=True, text=True)
(out/'assemble.log').write_text(assembled.stdout+assembled.stderr)
if assembled.returncode != 0:
    raise SystemExit('Fixture assembly failed')
rows = []

def link(name, flags, diagnostic=None, ld=None):
    target = out/(name+'.elf')
    target.unlink(missing_ok=True)
    cmd = [ld or a.ld, *flags, str(obj), '-o', str(target)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    if diagnostic:
        ok = result.returncode != 0 and diagnostic in result.stderr and not target.exists()
    else:
        ok = result.returncode == 0 and target.is_file() and target.stat().st_size > 0
    rows.append(dict(name=name, command=cmd, rc=result.returncode, ok=ok))
    return target

modes = ['task', 'job', 'job-initialize']
for mode in modes:
    flag = '--spurs-'+mode
    for name, flags in [('before', ['-r', flag]), ('after', [flag, '-r'])]:
        link(mode+'-relocatable-'+name, flags,
             'SPURS mode does not support shared or relocatable links')
    link(mode+'-repeat', [flag, flag])
for left, right in itertools.permutations(modes, 2):
    link(left+'-conflicts-'+right, ['--spurs-'+left, '--spurs-'+right],
         'conflicting SPURS modes')
ordinary = link('ordinary-relocatable', ['-r'])
if a.stock_ld:
    stock = link('ordinary-relocatable-stock', ['-r'], ld=a.stock_ld)
    rows.append(dict(name='ordinary-relocatable-byte-identical',
                     ok=ordinary.read_bytes() == stock.read_bytes()))
for row in rows:
    print(row['name'], 'PASS' if row['ok'] else 'FAIL')
(out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
raise SystemExit(0 if all(row['ok'] for row in rows) else 1)
