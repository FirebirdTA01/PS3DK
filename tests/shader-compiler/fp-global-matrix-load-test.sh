#!/usr/bin/env bash
# File-scope FP matrix loads must retain the registered row sources.
# PS3_475: global and entry-parameter spellings have identical ucode;
# 3x3 records are 1059/1047, 4x4 records are 1064/1048 (parent/row).
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 - "$compiler" <<'PY'
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
failures = []

def require(condition, message):
    if not condition:
        failures.append(message)

def decode(path):
    data = path.read_bytes()
    _, _, total, count, table, _, usize, uoff = struct.unpack_from('>8I', data)
    require(total == len(data), f'{path.name}: container length')
    records = {}
    for i in range(count):
        record = struct.unpack_from('>12I', data, table + i*48)
        name = data[record[4]:data.index(b'\0', record[4])].decode() if record[4] else ''
        records[name] = record
    return data, records, data[uoff:uoff+usize]

with tempfile.TemporaryDirectory(prefix='ps3dk-global-matrix-') as temp:
    root = Path(temp)
    for width, parent_type, row_type in ((3, 1059, 1047), (4, 1064, 1048)):
        result = 'float4(mul(M, t.xyz), 1.0)' if width == 3 else 'mul(M, t)'
        bodies = {
            'global': f'uniform float{width}x{width} M;\n'
                      f'float4 main(float4 t : TEXCOORD0) : COLOR {{ return {result}; }}\n',
            'parameter': f'float4 main(float4 t : TEXCOORD0, uniform float{width}x{width} M)'
                         f' : COLOR {{ return {result}; }}\n',
        }
        outputs = {}
        for spelling, source in bodies.items():
            tag = f'mat{width}-{spelling}'
            src, output = root/(tag+'.cg'), root/(tag+'.bin')
            src.write_text(source)
            try:
                run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container',
                                      str(output), str(src)], capture_output=True, timeout=15)
            except subprocess.TimeoutExpired:
                failures.append(f'{tag}: timed out')
                continue
            if run.returncode != 0:
                failures.append(f'{tag}: compiler exit {run.returncode}: '
                                + run.stderr.decode(errors='replace').strip())
                continue
            data, records, ucode = decode(output)
            outputs[spelling] = ucode
            require('M' in records and records['M'][0] == parent_type,
                    f'{tag}: matrix parent type must be {parent_type}')
            offsets = set()
            for row in range(width):
                name = f'M[{row}]'
                if name not in records:
                    failures.append(f'{tag}: missing row {name}')
                    continue
                record = records[name]
                require(record[0] == row_type, f'{tag}: {name} type must be {row_type}')
                require(record[5] == 0, f'{tag}: {name} must not invent a default')
                ec = record[6]
                if not ec:
                    failures.append(f'{tag}: {name} has no embedded row locations')
                    continue
                count = struct.unpack_from('>I', data, ec)[0]
                locations = struct.unpack_from(f'>{count}I', data, ec+4)
                require(bool(locations), f'{tag}: {name} has no patch locations')
                for location in locations:
                    require(location % 16 == 0 and location+16 <= len(ucode),
                            f'{tag}: {name} patch location {location} is outside ucode')
                    require(location not in offsets, f'{tag}: matrix rows alias location {location}')
                    offsets.add(location)
        if len(outputs) == 2:
            require(outputs['global'] == outputs['parameter'],
                    f'mat{width}: global and parameter ucode differ')

if failures:
    for failure in failures:
        print('FAIL: ' + failure, file=sys.stderr)
    sys.exit(1)
print('PASS: fp-global-matrix-load (3x3/4x4 ucode, types, distinct row patch locations)')
PY
