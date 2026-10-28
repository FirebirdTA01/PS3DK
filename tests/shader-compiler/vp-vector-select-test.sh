#!/usr/bin/env bash
# A vertex program select with a VECTOR condition (PSGL's fixed pipeline:
# `otc = texunit.genMode == float4(1,1,1,1) ? mul(v, texunit.genPlane) : otc`)
# was refused.  Measured on the established compiler: the comparison writes all four
# condition lanes (SEQC), the false arm is written, then the true arm under
# (NE0) - each lane by its own condition.  Ours lowers it the same way.
# Judged by value per lane, against independent arithmetic, with conditions
# that differ lane by lane; a scalar condition keeps its broadcast path.
#
# Usage: tests/shader-compiler/vp-vector-select-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-vector-select: FAIL: compiler required"; exit 1; }
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
H = 'uniform float4 gm; uniform float4 a;' + NL
M = 'float4 main(float4 p : POSITION) : POSITION { '

def e_eq(p, gm, a):
    return [f32(f32(p[j]) * 2.0) if gm[j] == 1.0 else a[j] for j in range(4)]
def e_mixed(p, gm, a):
    t = [p[j] if gm[j] > 0 else a[j + 1] for j in range(3)] + [1.0]
    u = [f32(p[j] * 3.0) if gm[j] != a[j] else a[j] for j in range(4)]
    return [f32(t[j] + u[j]) for j in range(4)]
def e_scalar(p, gm, a):
    return [f32(p[j] * 2.0) for j in range(4)] if gm[0] > 0 else list(a)

ROWS = [
    ('equal_vec4', M + 'return gm == float4(1,1,1,1) ? p * 2 : a; }', e_eq),
    ('vec3_and_notequal', M + 'float3 t = gm.xyz > float3(0,0,0) ? p.xyz : a.yzw; '
                          'float4 u = gm != a ? p * 3 : a; return float4(t, 1) + u; }', e_mixed),
    ('scalar_condition', M + 'return gm.x > 0 ? p * 2 : a; }', e_scalar),
]

def agrees(got, want):
    if got is None or len(got) != 4:
        return False
    if not all(isinstance(x, float) and math.isfinite(x) for x in got):
        return False
    return all(abs(x - y) <= 1e-5 * max(1.0, abs(y)) for x, y in zip(got, want))
assert not agrees([math.nan] * 4, [0.0] * 4) and not agrees([], [0.0] * 4), 'instrument control'

fails = []
rnd = random.Random(5)
for name, src, fn in ROWS:
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(H + src + NL)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d): %s' % (name, p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
        print('  %-20s REFUSED' % name); continue
    wrong, mixed = 0, 0
    for _ in range(40):
        gm = [rnd.choice([1.0, 0.0, -1.0, 2.0]) for _ in range(4)]
        a = [f32(rnd.choice([1.0, 0.0, -1.0, 2.0, rnd.uniform(-3, 3)])) for _ in range(4)]
        pos = [f32(rnd.uniform(-2, 2)) for _ in range(4)]
        mixed += len(set(x == 1.0 for x in gm)) > 1
        try:
            got = ev.evaluate(o.read_bytes(), {'gm': gm, 'a': a}, inputs={0: pos}, binary32=True,
                              predication=True).get(0)
        except AssertionError as e:
            fails.append('%s: instrument refused the program: %s' % (name, e)); wrong = -1; break
        if not agrees(got, fn(pos, gm, a)):
            wrong += 1
            if wrong == 1: fails.append('%s: gm=%s a=%s p=%s got %s want %s' % (name, gm, a, pos, got, fn(pos, gm, a)))
    assert mixed > 5, 'too few lane-mixed conditions'
    print('  %-20s %s' % (name, 'ok' if wrong == 0 else 'UNJUDGED' if wrong < 0 else '%d WRONG' % wrong))

for f in fails: print('vp-vector-select: FAIL', f)
print('vp-vector-select: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
