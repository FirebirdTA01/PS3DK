#!/usr/bin/env bash
# Interim containment for t_49f3cc72. General VP global register(CN) writes
# reflection for the pin while reading an automatic register. Legacy honors
# the pin; :CN and entry parameters are consistently auto-allocated today.
# Remove the interim refusal rows when explicit general allocation lands.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 - "$compiler" "$repo_root/tests/shader-compiler" <<'PY'
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0, sys.argv[2])
from vp_words import decode

compiler = str(Path(sys.argv[1]).resolve())
failures = []
diagnostic = "nv40-general-vp: explicit file-scope register(C9) on 'u' is not yet lowered consistently with reflection (t_49f3cc72); refusing"

def require(ok, message):
    if not ok:
        failures.append(message)

def records(blob):
    header = struct.unpack_from('>8I', blob)
    result = {}
    for i in range(header[3]):
        row = struct.unpack_from('>12I', blob, header[4]+48*i)
        name = blob[row[4]:blob.index(0, row[4])].decode()
        result[name] = row
    return header, result

def check_vp(label, blob, expected):
    _, rows = records(blob)
    words, error = decode(blob)
    require(error is None, label+': '+str(error))
    for name, register in expected.items():
        row = rows.get(name)
        require(row is not None and row[3] == register,
                label+': wrong reflected register for '+name)
    # These controls have one uniform and no literals: every constant read
    # must be the uniform's reflected register, including the legacy pin.
    reads = {int(n) for line in words for n in re.findall(r'\bC(\d+)\.', line)}
    require(reads == set(expected.values()), label+': decoded reads '+str(reads))

def check_fp(label, blob):
    header, rows = records(blob)
    row = rows.get('u')
    require(row is not None and row[1] == 3256 and row[3] == 0xffffffff,
            label+': unexpected FP uniform binding metadata')
    if row is None or not row[6]:
        failures.append(label+': missing embedded constant record')
        return
    count = struct.unpack_from('>I', blob, row[6])[0]
    patches = set(struct.unpack_from('>'+str(count)+'I', blob, row[6]+4))
    # FP constant sources consume the 16-byte block immediately after their
    # instruction; walk actual source tags, skipping those data blocks.
    consumed = set()
    offset = 0
    while offset < header[6]:
        raw = struct.unpack_from('>4I', blob, header[7]+offset)
        words = [((w << 16) | (w >> 16)) & 0xffffffff for w in raw]
        offset += 16
        if any((word & 3) == 2 for word in words[1:]):
            consumed.add(offset)
            offset += 16
    require(patches and patches == consumed,
            label+': reflected patches '+str(patches)+' != consumed '+str(consumed))

with tempfile.TemporaryDirectory(prefix='ps3dk-binding-refusal-') as directory:
    root = Path(directory)
    def run(label, source, *, profile='sce_vp_rsx', legacy=False, refusal=False, reg=467):
        path, output = root/(label+'.cg'), root/(label+'.bin')
        path.write_text(source)
        args = [compiler, '-p', profile, '--legacy-lowering' if legacy else '--general-lowering',
                '--emit-container', str(output), str(path)]
        try:
            result = subprocess.run(args, capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            failures.append(label+': timed out')
            return
        log = result.stdout+result.stderr
        if refusal:
            require(result.returncode == 1 and diagnostic in log and not output.exists(),
                    label+': expected exit 1 with named refusal and no container, got '+
                    str(result.returncode)+'\n'+log)
            return
        if result.returncode != 0 or not output.exists():
            failures.append(label+': expected success, got '+str(result.returncode)+'\n'+log)
            return
        if profile == 'sce_vp_rsx':
            check_vp(label, output.read_bytes(), {'u':reg})
        else:
            check_fp(label, output.read_bytes())

    for type_name in ('float', 'float4'):
        global_pin = f'uniform {type_name} u:register(C9); float4 main(float4 p:POSITION):POSITION {{return p*u;}}'
        run(type_name+'-global-pin', global_pin, refusal=True)
        run(type_name+'-legacy-pin', global_pin, legacy=True, reg=9)
        run(type_name+'-global-semantic', global_pin.replace('register(C9)', 'C9'))
        for spelling in ('register(C9)', 'C9'):
            run(type_name+'-entry-'+spelling,
                f'float4 main(float4 p:POSITION,uniform {type_name} u:{spelling}):POSITION {{return p*u;}}')
            run(type_name+'-fp-global-'+spelling,
                f'uniform {type_name} u:{spelling}; float4 main(float4 p:TEXCOORD0):COLOR {{return p*u;}}',
                profile='sce_fp_rsx')
            run(type_name+'-fp-entry-'+spelling,
                f'float4 main(float4 p:TEXCOORD0,uniform {type_name} u:{spelling}):COLOR {{return p*u;}}',
                profile='sce_fp_rsx')
    run('matrix-pin', 'uniform float4x4 u:register(C9); float4 main(float4 p:POSITION):POSITION {return mul(u,p);}', refusal=True)
    run('vector-array-pin', 'uniform float4 u[2]:register(C9); float4 main(float4 p:POSITION):POSITION {return p*u[1];}', refusal=True)
    run('matrix-array-pin', 'uniform float4x4 u[2]:register(C9); float4 main(float4 p:POSITION):POSITION {return mul(u[1],p);}', refusal=True)
    # The unused pin still makes the container skip a slot that the general
    # allocator consumes. A check only on loads of u misses the wrong v read.
    run('unused-pin', 'uniform float4 u:register(C9); uniform float4 v; float4 main(float4 p:POSITION):POSITION {return p*v;}', refusal=True)
    run('struct-entry', 'uniform float4 u:register(C9); struct Input {float4 p:POSITION;}; float4 main(Input i):POSITION {return i.p*u;}', refusal=True)

if failures:
    for failure in failures:
        print('FAIL: '+failure, file=sys.stderr)
    sys.exit(1)
print('PASS: vp-explicit-binding-refusal (general refusals, legacy pin, consistent VP/FP controls)')
PY
