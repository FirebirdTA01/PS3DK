#!/usr/bin/env bash
# Vertex arithmetic shape: a single-use product feeding an add is one MAD
# (when the MAD reads at most one input and one constant), the last
# producer of a returned value or a struct output member writes the output
# register itself, a temp-to-temp copy is read through its source, and a
# MOV the allocator coalesced onto its own register is dropped.  Equal
# constants share one value, so a repeated `1.0 / k.x` is one RCP, and a
# lane copy out of a temp has the temp's producer write the lane itself
# (`RCP R0.xz` builds two lanes of a vector at once).  Each row's
# instruction count is held to a budget; where the budget is the
# established compiler's count (measured with the established compiler) the row says so.  Every row
# is also judged by value against independent binary32 arithmetic.
#
# Usage: tests/shader-compiler/vp-arith-shape-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-arith-shape: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import math, random, struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import vp_pow_vector_check as ev
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
NL = chr(10)
ROWS = [
 ('two_inputs', 'float4 main(float4 a : ATTR0, float4 b : ATTR1) : POSITION { return a + b * 2; }'),
 ('weighted_chain', 'struct S { float4 a; float4 b; float4 c; float4 d; float4 e; float4 f; };' + NL +
  'float4 main(S s, float4 col : COLOR) : POSITION { return s.a + s.b * 2 + s.c * 3 + s.d * 4 + s.e * 5 + s.f * 6 + col * 7; }'),
 ('uniform_mad', 'float4 main(float4 p : POSITION, uniform float4 k, uniform float4 m) : POSITION { return p * k + m; }'),
 ('struct_out', 'struct O { float4 p : POSITION; float4 t : TEXCOORD0; float4 u : TEXCOORD1; };' + NL +
  'O main(float4 p : POSITION, float4 n : NORMAL, uniform float4 k) { O o; o.p = p; o.t = n * k + p; o.u = -n * 0.5 + k; return o; }'),
 ('shared_product', 'float4 main(float4 p : POSITION, uniform float4 k, uniform float4 m) : POSITION { float4 t = p * k; return (t + m) * t; }'),
 ('copy_chain', 'struct O { float4 p : POSITION; float4 t : TEXCOORD0; };' + NL +
  'O main(float4 p : POSITION, uniform float4 k) { O o; float d = 1.0 / k.x; o.p = p; o.t = p.xxxy + float4(-d, 0, d, -2.0 * d); return o; }'),
 ('shared_reciprocal', 'float4 main(float4 p : POSITION, uniform float4 k) : POSITION { float a = 1.0 / k.x; float b = 1.0 / k.x; return p * a + float4(b, 2.0 * b, b, 1.0); }'),
 ('loop_mad', 'float4 main(float4 p : POSITION, uniform float4 k, uniform int n) : POSITION { float4 a = p; for (int i = 0; i < n; i++) a = a * k + p; return a; }'),

]
# name -> (instruction budget, the established compiler's count)
BUDGET = {
    'two_inputs': (2, 2), 'weighted_chain': (7, 7), 'uniform_mad': (2, 2),
    'struct_out': (5, 5), 'shared_product': (3, 3), 'copy_chain': (5, 6), 'loop_mad': (10, 10),
    'shared_reciprocal': (4, 4),
}
HPOS, TEX0, TEX1 = 0, 7, 8

def vadd(a, b): return [f32(x + y) for x, y in zip(a, b)]
def vmul(a, b): return [f32(x * y) for x, y in zip(a, b)]
def vs(a, s): return [f32(x * s) for x in a]

def expect(name, v, u):
    k, m, n = u['k'], u['m'], int(u['n'][0])
    if name == 'two_inputs':
        return {HPOS: vadd(v[0], vs(v[1], 2.0))}
    if name == 'weighted_chain':
        acc = v[0]
        for attr, w in ((1, 2), (2, 3), (4, 4), (5, 5), (6, 6), (3, 7)):
            acc = vadd(acc, vs(v[attr], float(w)))
        return {HPOS: acc}
    if name == 'uniform_mad':
        return {HPOS: vadd(vmul(v[0], k), m)}
    if name == 'struct_out':
        return {HPOS: v[0], TEX0: vadd(vmul(v[2], k), v[0]),
                TEX1: vadd(vs([-x for x in v[2]], 0.5), k)}
    if name == 'shared_product':
        t = vmul(v[0], k)
        return {HPOS: vmul(vadd(t, m), t)}
    if name == 'copy_chain':
        d = f32(1.0 / k[0]); p = v[0]
        return {HPOS: p, TEX0: vadd([p[0], p[0], p[0], p[1]], [-d, 0.0, d, f32(-2.0 * d)])}
    if name == 'shared_reciprocal':
        r = f32(1.0 / k[0])
        return {HPOS: vadd(vs(v[0], r), [r, f32(2.0 * r), r, 1.0])}
    if name == 'loop_mad':
        a = v[0]
        for _ in range(n):
            a = vadd(vmul(a, k), v[0])
        return {HPOS: a}

def agrees(got, want):
    if got is None or len(got) != len(want):
        return False
    if not all(isinstance(x, float) and math.isfinite(x) for x in got):
        return False
    return all(abs(x - y) <= 1e-5 * max(1.0, abs(y)) for x, y in zip(got, want))
assert not agrees([math.nan] * 4, [0.0] * 4) and not agrees([], [0.0] * 4), 'instrument control'

fails = []
rnd = random.Random(17)
for name, src in ROWS:
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(src + NL)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d): %s' % (name, p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
        print('  %-15s REFUSED' % name); continue
    blob = o.read_bytes()
    count = struct.unpack_from('>I', blob, 24)[0] // 16
    budget, ref = BUDGET[name]
    if count > budget:
        fails.append('%s: %d instructions, budget %d (the reference: %d)' % (name, count, budget, ref))
    wrong = 0
    for _ in range(16):
        v = {i: [f32(rnd.uniform(-2, 2)) for _ in range(4)] for i in range(16)}
        u = {'k': [f32(rnd.uniform(0.5, 2)) for _ in range(4)],
             'm': [f32(rnd.uniform(-2, 2)) for _ in range(4)],
             'n': [float(rnd.randint(0, 4))] * 4}
        got = ev.evaluate(blob, u, inputs=v, binary32=True, predication=True, branches=True)
        want = expect(name, v, u)
        for out, vec in want.items():
            if not agrees(got.get(out), vec):
                wrong += 1
                if wrong == 1: fails.append('%s: output %d got %s want %s' % (name, out, got.get(out), vec))
    print('  %-15s %2d instructions (budget %d, reference %d) %s' % (
        name, count, budget, ref, 'ok' if wrong == 0 else '%d WRONG' % wrong))

for f in fails: print('vp-arith-shape: FAIL', f)
print('vp-arith-shape: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
