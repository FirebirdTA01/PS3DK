#!/usr/bin/env bash
# A two-dimensional local array (`float2 crd[4][4]`, lanczos12): `T a[2][3]`
# is an array of 2 arrays of 3 T - the FIRST bracket is the outer dimension -
# and `a[i][j]` with literal indices reads and writes element j of row i.
# Judged by value; a three-dimensional array and an out-of-range row or
# element are refused.
#
# Usage: tests/shader-compiler/local-2d-array-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "local-2d-array: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
NL = chr(10)
M = 'float4 main(float4 t : TEXCOORD0) : COLOR { '
VALUE = [
    ('rows_and_columns',
     M + 'float2 c[2][3]; c[0][0] = t.xy; c[0][2] = t.zw; c[1][2] = t.yx; '
         'return float4(c[0][0].x, c[0][2].y, c[1][2]); }',
     lambda t: [t[0], t[3], t[1], t[0]]),
    # Built from earlier elements, as lanczos12 builds its coordinates.
    ('chained',
     M + 'float2 c[3][2]; c[0][0] = t.xy; c[1][0] = c[0][0] + t.zw; c[2][1] = c[1][0] * 2.0; '
         'return float4(c[1][0], c[2][1]); }',
     lambda t: [f32(t[0] + t[2]), f32(t[1] + t[3]), f32(f32(t[0] + t[2]) * 2.0), f32(f32(t[1] + t[3]) * 2.0)]),
    ('lane_write',
     M + 'float3 c[2][2]; c[1][0] = t.xyz; c[1][0].y = 9.0; return float4(c[1][0], 1); }',
     lambda t: [t[0], 9.0, t[2], 1.0]),
    # A static loop over both indices (expanded, so the indices are literal).
    ('static_loops',
     M + 'float c[2][3]; for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) c[i][j] = t.x * i + t.y * j; '
         'return float4(c[0][1], c[1][0], c[1][2], 1); }',
     lambda t: [t[1], t[0], f32(t[0] + f32(t[1] * 2.0)), 1.0]),
]
REFUSE = [
    ('three_dimensions', M + 'float c[2][2][2]; c[0][0][0] = t.x; return float4(c[0][0][0], 0, 0, 1); }', 'more than two dimensions'),
    # A two-dimensional array PARAMETER is refused: its rows would not be
    # rebound, so a caller row of the same name could be read (review: codex).
    ('param_2d_shadow', 'float h(float a[2][2]) { return a[0][0]; }' + NL +
                        M + 'float a[2][2]; float b[2][2]; a[0][0] = t.x; b[0][0] = t.y; return float4(h(b), 0, 0, 1); }',
     'two-dimensional array parameter'),
    ('param_2d_inout', 'void h(inout float a[2][2]) { a[0][0] = 9; }' + NL +
                       M + 'float a[2][2]; a[0][0] = t.x; h(a); return float4(a[0][0], 0, 0, 1); }', 'member-array-storage'),
    ('row_out_of_range', M + 'float2 c[2][3]; c[2][0] = t.xy; return float4(c[0][0], 0, 1); }', 'error'),
    ('element_out_of_range', M + 'float2 c[2][3]; c[0][3] = t.xy; return float4(c[0][0], 0, 1); }', 'error'),
]
TS = [[0.25, -0.5, 1.5, 2.0], [-1.0, 0.75, 0.125, -3.0], [3.0, 0.0, -2.5, 0.5]]
fails = []
def compile_one(name, src):
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src + NL)
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=60)
    return p, o
for name, src, want_fn in VALUE:
    p, o = compile_one(name, src)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); print('  %-22s REFUSED' % name); continue
    wrong = 0
    for t in TS:
        t = [f32(x) for x in t]
        got = fp_eval.evaluate(o.read_bytes(), {'TEX0': t}); want = [f32(x) for x in want_fn(t)]
        if got != want:
            fails.append('%s at t=%s: got %s want %s' % (name, t, got, want)); wrong += 1
    print('  %-22s %s' % (name, 'ok' if not wrong else '%d WRONG' % wrong))
for name, src, text in REFUSE:
    p, o = compile_one(name, src)
    ok = p.returncode == 1 and not o.exists() and text in (p.stdout + p.stderr)
    print('  %-22s %s' % (name, 'refused' if ok else 'NOT refused as expected (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1 naming %r and no container' % (name, text))
for f in fails: print('local-2d-array: FAIL', f)
print('local-2d-array: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
