#!/usr/bin/env bash
# The vertex engine holds 544 instructions.  Measured on the established compiler: a
# 544-instruction program compiles, 545 is refused ("C9999: too many
# instruction words generated").  Ours never checked, and emitted a
# 1091-instruction program with a success exit.  Now refused by resource
# (vp-instruction-budget).
#
# Rows use `s = s * k[i] + p.yzwx;` with |k| <= 0.5, so every value stays
# bounded (|s| <= 2|p|) and is judged in binary32 against independent float32
# arithmetic (within 1e-5 relative: the VP MAD's rounding is not pinned
# here).  Each line is one MAD, as in the established compiler (one input, one
# constant), so the boundary is the established compiler's own count:
#   543 lines                     543 instructions   accepted, by value
#   543 lines + one multiply      544 instructions   accepted, by value (the limit)
#   545 lines                     545 instructions   refused by both
# Usage: tests/shader-compiler/vp-instruction-budget-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-instruction-budget: FAIL: compiler required"; exit 1; }
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
TAIL = 's = s * k[199];'

def source(n, tail=''):
    lines = ['uniform float4 k[200];', 'float4 main(float4 p : POSITION) : POSITION { float4 s = p;']
    lines += ['s = s * k[%d] + p.yzwx;' % (i % 200) for i in range(n)]
    return NL.join(lines + [tail + 'return s; }']) + NL

def expect(n, tail, p, k):
    s = list(p); y = p[1:] + p[:1]
    for i in range(n):
        s = [f32(f32(s[c] * k[i % 200][c]) + y[c]) for c in range(4)]
    if tail:
        s = [f32(s[c] * k[199][c]) for c in range(4)]
    return s

def compile_one(name, text):
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(text)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=300)
    return p, o

fails = []
for label, n, tail, want_count in (('543 instructions accepted', 543, '', 543),
                                   ('544 instructions accepted', 543, TAIL, 544)):
    p, o = compile_one('n%d%s' % (n, 't' if tail else ''), source(n, tail))
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (label, p.returncode)); print('  %-30s REFUSED' % label); continue
    blob = o.read_bytes(); count = struct.unpack_from('>8I', blob, 0)[6] // 16
    rnd = random.Random(n + len(tail)); wrong = 0
    for _ in range(3):
        pos = [f32(rnd.uniform(-1, 1)) for _ in range(4)]
        k = [[f32(rnd.uniform(-0.5, 0.5)) for _ in range(4)] for _ in range(200)]
        want = expect(n, tail, pos, k)
        assert all(math.isfinite(x) for x in want), 'expectation not finite'
        got = ev.evaluate(blob, {'k[%d]' % i: k[i] for i in range(200)}, inputs={0: pos}, binary32=True).get(0)
        if got is None or len(got) != 4 or not all(math.isfinite(x) and abs(x - y) <= 1e-5 * max(1.0, abs(y))
                                                  for x, y in zip(got, want)):
            wrong += 1
    ok = count == want_count and wrong == 0
    print('  %-30s %s' % (label, 'ok' if ok else '%d instructions, %d wrong' % (count, wrong)))
    if not ok: fails.append('%s: %d instructions, %d wrong' % (label, count, wrong))
for label, n in (('545 instructions refused (both)', 545),):
    p, o = compile_one('n%d' % n, source(n))
    ok = p.returncode == 1 and not o.exists() and 'vp-instruction-budget' in p.stdout + p.stderr
    print('  %-30s %s' % (label, 'refused' if ok else 'NOT refused (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1, no container and vp-instruction-budget' % label)
for f in fails: print('vp-instruction-budget: FAIL', f)
print('vp-instruction-budget: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
