#!/usr/bin/env bash
# A file-scope uniform array passed by name to a helper's array parameter:
# indexing the parameter reads the global's elements (measured on the
# reference; lcd-grid-v2 passes `const float coeffs_x[] = {...}` to
# `float intsmear_func(float z, float coeffs[7])`).  Before, the inliner
# copied the argument's (absent) element values and the program refused
# with an unresolved operand.  Values are judged by arithmetic with the
# initialisers as the compiled defaults.
#
# Usage: tests/shader-compiler/helper-uniform-array-arg-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "helper-uniform-array-arg: FAIL: compiler required"; exit 1; }
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
S = 'float s(float c[3], float x) { return c[0] + c[1] * x + c[2] * x * x; }' + NL
MAIN = 'float4 main(float4 t : TEXCOORD0) : COLOR { return float4(s(k, t.x), k[2], 0, 1); }' + NL
poly = lambda x: f32(f32(1.0 + f32(-0.5 * x)) + f32(f32(0.25 * x) * x))
ROWS = [
    ('unsized_const', 'const float k[] = { 1.0, -0.5, 0.25 };' + NL + S + MAIN,
     lambda t: [poly(t[0]), 0.25, 0.0, 1.0]),
    ('sized_const', 'const float k[3] = { 1.0, -0.5, 0.25 };' + NL + S + MAIN,
     lambda t: [poly(t[0]), 0.25, 0.0, 1.0]),
    ('uniform_with_defaults', 'uniform float k[3] = { 1.0, -0.5, 0.25 };' + NL + S + MAIN,
     lambda t: [poly(t[0]), 0.25, 0.0, 1.0]),
    ('passed_on', 'const float k[3] = { 1.0, -0.5, 0.25 };' + NL +
     'float g(float c[3], float x) { return c[2] * x; }' + NL +
     'float s(float c[3], float x) { return c[0] + c[1] * x + g(c, x * x); }' + NL + MAIN,
     lambda t: [f32(f32(1.0 + f32(-0.5 * t[0])) + f32(0.25 * f32(t[0] * t[0]))), 0.25, 0.0, 1.0]),
    ('helper_local_array', 'const float k[3] = { 1.0, -0.5, 0.25 };' + NL +
     'float s(float c[3], float x) { float c2[3] = { 9, 9, 9 }; return c[0] + c2[1] * x; }' + NL +
     'float4 main(float4 t : TEXCOORD0) : COLOR { return float4(s(k, t.x), 0, 0, 1); }' + NL,
     lambda t: [f32(1.0 + f32(9.0 * t[0])), 0.0, 0.0, 1.0]),
]
fails = []
for name, src, want_fn in ROWS:
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src)
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=60)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); print('  %-22s REFUSED' % name); continue
    ok = True
    for t in ([0.5, 0, 0, 0], [-1.25, 0, 0, 0], [2.0, 0, 0, 0]):
        t = [f32(x) for x in t]
        got = fp_eval.evaluate(o.read_bytes(), {'TEX0': t}); want = want_fn(t)
        if any(abs(a - b) > 1e-6 * max(1.0, abs(b)) for a, b in zip(got, want)):
            fails.append('%s: got %s want %s at x=%s' % (name, got, want, t[0])); ok = False
    print('  %-22s %s' % (name, 'ok' if ok else 'WRONG'))
for f in fails: print('helper-uniform-array-arg: FAIL', f)
print('helper-uniform-array-arg: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
