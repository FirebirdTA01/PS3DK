#!/usr/bin/env python3
"""The linker accepts an exact AX GUID reservation and rejects extra flags."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
from spurs_guid_check import inspect

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--ld')
p.add_argument('--as', dest='assembler')
p.add_argument('--output', type=Path, default=Path('build/spurs-guid-reservation'))
a = p.parse_args()
dev = os.environ.get('PS3DEV')
if not a.ld and not dev:
    print('SPURS GUID reservation: SKIP (set PS3DEV or --ld and --as)')
    raise SystemExit(0)
a.ld = a.ld or str(Path(dev)/'spu/bin/spu-elf-ld')
a.assembler = a.assembler or (str(Path(dev)/'spu/bin/spu-elf-as') if dev else None)
if not a.assembler:
    p.error('--as is required without PS3DEV')
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
source, obj = out/'fixture.s', out/'fixture.o'
source.write_text('.section .SpuGUID,"ax",@progbits\n.global __SPU_GUID\n'
                  '__SPU_GUID:\n.space 16\n.text\n.global _start\n_start:\n nop\n')
subprocess.run([a.assembler, str(source), '-o', str(obj)], check=True)
data = bytearray(obj.read_bytes())
offset = struct.unpack_from('>I', data, 32)[0]
stride, count, si = struct.unpack_from('>HHH', data, 46)
headers = [struct.unpack_from('>10I', data, offset+i*stride) for i in range(count)]
names = data[headers[si][4]:headers[si][4]+headers[si][5]]
index = next(i for i,h in enumerate(headers)
             if names[h[0]:].split(b'\0')[0] == b'.SpuGUID')
assert headers[index][2] == 6
struct.pack_into('>I', data, offset+index*stride+8, 6 | 0x400)  # extra SHF_TLS
mutant = out/'extra-tls.o'; mutant.write_bytes(data)
rows = []
for mode in ['job', 'job-initialize']:
    for label, input_obj in [('valid', obj), ('extra-flags', mutant)]:
        name = mode+'-'+label
        target = out/(name+'.elf'); target.unlink(missing_ok=True)
        cmd = [a.ld, '--spurs-'+mode, str(input_obj), '-o', str(target)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        row = dict(name=name, rc=result.returncode, command=cmd, ok=False)
        if label == 'valid' and result.returncode == 0:
            row['identity'] = inspect(target)
            row['ok'] = True
        elif label == 'extra-flags':
            row['ok'] = (result.returncode != 0 and '.SpuGUID must be' in result.stderr
                         and not target.exists())
        rows.append(row)
for row in rows:
    print(row['name'], 'PASS' if row['ok'] else 'FAIL')
(out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
raise SystemExit(0 if all(r['ok'] for r in rows) else 1)
