#!/usr/bin/env bash
# Matrix element selectors and matrix-to-vector conversion, as Cg defines
# them (measured on the reference):
#   m._m00_m01_m10_m11 (zero-based) and m._11_12_21_22 (one-based), one to
#   four elements, repeats allowed; an element outside the matrix is C1031;
#   mixing the forms is C1048;
#   float4(float2x2) is the components row-major (m00 m01 m10 m11);
#   a vector that does not hold exactly the matrix's components (float3 of a
#   float2x3, float4 of a float3x3) is refused.
# A select between two matrices (if/else assigning a matrix on both arms)
# is per-row selects.
# A write through a matrix selector is accepted by the reference but not
# lowered here yet, so it stays a named refusal.
#
# Usage: tests/shader-compiler/matrix-swizzle-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "matrix-swizzle: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
H = 'float4 main(float4 t : TEXCOORD0) : COLOR { '
M3 = 'float3x3 m = float3x3(t.xyz, t.yzw, t.zwx); '
VALUE = [
    ('zero_based_4', H + M3 + 'return m._m00_m01_m10_m11; }', lambda t: [t[0], t[1], t[1], t[2]]),
    ('one_based_4',  H + M3 + 'return m._11_12_21_22; }',     lambda t: [t[0], t[1], t[1], t[2]]),
    ('scalar',       H + M3 + 'return float4(m._m21, 0, 0, 1); }', lambda t: [t[3], 0, 0, 1]),
    ('repeat',       H + M3 + 'return m._m00_m00_m22_m22; }', lambda t: [t[0], t[0], t[0], t[0]]),
    ('reorder_3',    H + M3 + 'return float4(m._m22_m02_m11, 1); }', lambda t: [t[0], t[2], t[2], 1]),
    ('flatten_2x2',  H + 'float2x2 m = float2x2(t.x, t.y, t.z, t.w); return float4(m) * float4(1, 2, 3, 4); }',
                     lambda t: [t[0], f32(t[1] * 2), f32(t[2] * 3), f32(t[3] * 4)]),
    # An if/else that assigns a matrix on both arms selects the whole matrix
    # (lowered as per-row selects; measured identical to the reference).
    ('select_2x2', H + 'float2x2 m; if (t.x > 0.5) m = float2x2(t.x, t.y, t.z, t.w); '
                   'else m = float2x2(t.w, -t.z, 2, t.y); float2 v = mul(m, float2(1, 0.5)); return float4(v, m[1]); }',
     lambda t: (lambda m: [f32(m[0] + f32(0.5 * m[1])), f32(m[2] + f32(0.5 * m[3])), m[2], m[3]])(
         [t[0], t[1], t[2], t[3]] if t[0] > 0.5 else [t[3], -t[2], 2.0, t[1]])),
    ('row_4x4',      H + 'float4x4 q = float4x4(t, t.yzwx, t.zwxy, t.wxyz); return q._m30_m31_m32_m33; }',
                     lambda t: [t[3], t[0], t[1], t[2]]),
]
REFUSE = [
    ('out_of_range', H + M3 + 'return float4(m._m33, 0, 0, 1); }', 'C1031'),
    ('one_based_out_of_range', H + M3 + 'return float4(m._44, 0, 0, 1); }', 'C1031'),
    ('mixed_forms', H + M3 + 'return float4(m._m00_12, 0, 1); }', 'C1048'),
    ('flatten_count_mismatch', H + 'float3x3 m = float3x3(t.xyz, t.yzw, t.zwx); return float4(m); }', 'error'),
    ('write_through_selector', H + M3 + 'm._m00_m11 = float2(5, 6); return float4(m._m00, m._m11, 0, 1); }', 'not assignable'),
]
fails = []
def compile_one(name, src):
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src + chr(10))
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=60)
    return p, o
for name, src, want_fn in VALUE:
    p, o = compile_one(name, src)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); continue
    for t in ([0.25, -0.5, 1.5, 2.0], [-1.0, 0.75, 0.125, -3.0]):
        t = [f32(x) for x in t]
        got = fp_eval.evaluate(o.read_bytes(), {'TEX0': t}); want = [f32(x) for x in want_fn(t)]
        if got != want:
            fails.append('%s: got %s want %s for %s' % (name, got, want, t))
    print('  %-24s value rows done' % name)
for name, src, text in REFUSE:
    p, o = compile_one(name, src)
    ok = p.returncode == 1 and not o.exists() and text in (p.stdout + p.stderr)
    print('  %-24s %s' % (name, 'refused' if ok else 'NOT refused as expected (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1 naming %r and no container' % (name, text))
for f in fails: print('matrix-swizzle: FAIL', f)
print('matrix-swizzle: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
