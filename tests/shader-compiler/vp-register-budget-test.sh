#!/usr/bin/env bash
# The vertex engine has 32 temporary registers.  The reference refuses a
# program that needs more ("C6001: Temporary register limit of 32 exceeded").
# Ours encoded registers past R31 in the 6-bit field and reported success - an
# invalid program (PSGL's fixed pipeline reached R61).  Now refused by
# resource (vp-register-budget).
#
# Rows: N values each read twice, far apart (all live at once).  N=8 compiles,
# writes no register past R31 and is judged by value; N=34 is refused like the
# reference.  N=20 the reference compiles in 20 registers; ours schedules
# with more pressure and refuses it - a named gap, pinned here so a scheduler
# that closes it shows up.
#
# Usage: tests/shader-compiler/vp-register-budget-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-register-budget: FAIL: compiler required"; exit 1; }
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

def source(n):
    lines = ['uniform float4 k[%d];' % n, 'float4 main(float4 p : POSITION) : POSITION {']
    for i in range(n):
        lines.append('float4 a%d = p * k[%d] + float4(%d.0, %d.5, 0, 1);' % (i, i, i, i))
    lines.append('float4 s = a0;')
    for i in range(1, n):
        lines.append('s = s * a%d + a%d.yzwx;' % (i, i))
    for i in range(n):
        lines.append('s = s + a%d.wzyx * a%d.x;' % (n - 1 - i, i))
    lines.append('return s; }')
    return NL.join(lines) + NL

def expect(n, p, k):
    a = [[f32(f32(p[c] * k[i][c]) + [float(i), i + 0.5, 0.0, 1.0][c]) for c in range(4)] for i in range(n)]
    s = list(a[0])
    for i in range(1, n):
        y = a[i][1:] + a[i][:1]
        s = [s[c] * a[i][c] + y[c] for c in range(4)]
    for i in range(n):
        w = a[n - 1 - i][::-1]
        s = [s[c] + w[c] * a[i][0] for c in range(4)]
    return s

def compile_one(n):
    s, o = work / ('b%d.cg' % n), work / ('b%d.vpo' % n)
    s.write_text(source(n))
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    return p, o

def max_temp(blob):
    h = struct.unpack_from('>8I', blob, 0)
    w = struct.unpack_from('>%dI' % (h[6] // 4), blob, h[7]); top = -1
    for i in range(0, len(w), 4):
        vec, sca = (w[i] >> 15) & 63, (w[i + 3] >> 7) & 63
        if vec != 63: top = max(top, vec)
        if sca != 63: top = max(top, sca)
    return top

fails = []
p, o = compile_one(8)
if p.returncode != 0 or not o.exists():
    fails.append('N=8: refused (rc %d)' % p.returncode); print('  %-34s REFUSED' % 'N=8 within budget')
else:
    blob = o.read_bytes(); top = max_temp(blob)
    rnd = random.Random(8); wrong = 0
    for _ in range(6):
        pos = [f32(rnd.uniform(-1, 1)) for _ in range(4)]
        k = [[f32(rnd.uniform(-1, 1)) for _ in range(4)] for _ in range(8)]
        got = ev.evaluate(blob, {'k[%d]' % i: k[i] for i in range(8)}, inputs={0: pos}).get(0)
        want = expect(8, pos, k)
        if got is None or len(got) != 4 or not all(math.isfinite(x) and abs(x - y) <= 1e-3 * max(1.0, abs(y))
                                                  for x, y in zip(got, want)):
            wrong += 1
    ok = top <= 31 and wrong == 0
    print('  %-34s %s' % ('N=8 within budget', 'ok (R%d)' % top if ok else 'R%d, %d wrong' % (top, wrong)))
    if not ok: fails.append('N=8: max temp R%d, %d wrong values' % (top, wrong))
for n, label in ((34, 'N=34 refused like the reference'), (20, 'N=20 refused (named gap)')):
    p, o = compile_one(n)
    ok = p.returncode == 1 and not o.exists() and 'vp-register-budget' in p.stdout + p.stderr
    print('  %-34s %s' % (label, 'refused' if ok else 'NOT refused (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1, no container and vp-register-budget' % label)
for f in fails: print('vp-register-budget: FAIL', f)
print('vp-register-budget: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
