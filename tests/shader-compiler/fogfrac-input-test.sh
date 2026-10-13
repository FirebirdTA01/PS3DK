#!/usr/bin/env bash
# A fragment input with the FOGFRAC semantic (the SDK fw_fog:
# `float fogparam : FOGFRAC`) reads lane Y of the fog input register,
# f[FOGC].y, and its parameter record carries CG_FOGCOORD (3156) like FOG
# (measured on the reference).  It used to be left unbound ("operand could
# not be resolved").  Judged by value and by the record.
#
# Usage: tests/shader-compiler/fogfrac-input-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fogfrac-input: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
src = work / 'f.cg'; out = work / 'f.bin'
src.write_text('float4 main(float2 t : TEXCOORD0, float f : FOGFRAC) : COLOR { return float4(f, t.x * f, 0.5, 1); }' + chr(10))
p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(out), str(src)],
                   capture_output=True, text=True, timeout=60)
fails = []
if p.returncode != 0 or not out.exists():
    fails.append('refused (rc %d): %s' % (p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
else:
    blob = out.read_bytes()
    count, table = struct.unpack_from('>2I', blob, 12)
    res = None
    for i in range(count):
        w = struct.unpack_from('>12I', blob, table + 48 * i)
        if blob[w[4]:blob.index(0, w[4])].decode() == 'f': res = w[1]
    if res != 3156:
        fails.append('FOGFRAC record resource %r, want 3156 (CG_FOGCOORD)' % res)
    for fog in ([0.9, 0.25, -1.0, 2.0], [-0.5, 0.75, 0.5, 0.0]):
        t = [0.5, 0.0, 0.0, 0.0]
        got = fp_eval.evaluate(blob, {'TEX0': t, 'FOGC': [f32(x) for x in fog]})
        want = [fog[1], f32(t[0] * fog[1]), 0.5, 1.0]
        if got != want:
            fails.append('fog=%s: got %s want %s (FOGFRAC is f[FOGC].y)' % (fog, got, want))
for f in fails: print('fogfrac-input: FAIL', f)
print('fogfrac-input: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
