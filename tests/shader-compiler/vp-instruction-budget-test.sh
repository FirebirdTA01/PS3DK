#!/usr/bin/env bash
# The vertex engine holds 544 instructions.  Measured on the reference: a
# 544-instruction program compiles, 545 is refused ("C9999: too many
# instruction words generated").  Ours never checked, and emitted a
# 1091-instruction program with a success exit.  Now refused by resource
# (vp-instruction-budget).
#
# Rows (`s = s * k[i] + s.yzwx;` repeated): our code generation takes two
# instructions per line where the reference takes one MAD, so the boundary is
# pinned on OUR count - 271 lines (543 instructions) compile and are judged
# by value, 272 lines (545) are refused although the reference fits them in
# 272 (a named gap: re-derive when the MAD is fused); 545 lines are refused
# by both.
#
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
NL = chr(10)

def source(n):
    lines = ['uniform float4 k[200];', 'float4 main(float4 p : POSITION) : POSITION { float4 s = p;']
    lines += ['s = s * k[%d] + s.yzwx;' % (i % 200) for i in range(n)]
    return NL.join(lines + ['return s; }']) + NL

def compile_one(n):
    s, o = work / ('n%d.cg' % n), work / ('n%d.vpo' % n)
    s.write_text(source(n))
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=300)
    return p, o

fails = []
p, o = compile_one(271)
if p.returncode != 0 or not o.exists():
    fails.append('271 lines: refused (rc %d)' % p.returncode); print('  %-30s REFUSED' % '271 lines accepted')
else:
    blob = o.read_bytes(); count = struct.unpack_from('>8I', blob, 0)[6] // 16
    rnd = random.Random(271); wrong = 0
    for _ in range(3):
        pos = [rnd.uniform(-1, 1) for _ in range(4)]
        k = [[rnd.uniform(0.2, 0.6) for _ in range(4)] for _ in range(200)]
        s = list(pos)
        for i in range(271):
            y = s[1:] + s[:1]
            s = [s[c] * k[i % 200][c] + y[c] for c in range(4)]
        got = ev.evaluate(blob, {'k[%d]' % i: k[i] for i in range(200)}, inputs={0: pos}).get(0)
        if got is None or len(got) != 4 or not all(math.isfinite(x) and abs(x - y) <= 1e-3 * max(1.0, abs(y))
                                                  for x, y in zip(got, s)):
            wrong += 1
    ok = count <= 544 and wrong == 0
    print('  %-30s %s' % ('271 lines accepted', 'ok (%d instructions)' % count if ok else '%d instructions, %d wrong' % (count, wrong)))
    if not ok: fails.append('271 lines: %d instructions, %d wrong' % (count, wrong))
for n, label in ((272, '272 lines refused (named gap)'), (545, '545 lines refused like the reference')):
    p, o = compile_one(n)
    ok = p.returncode == 1 and not o.exists() and 'vp-instruction-budget' in p.stdout + p.stderr
    print('  %-30s %s' % (label, 'refused' if ok else 'NOT refused (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1, no container and vp-instruction-budget' % label)
for f in fails: print('vp-instruction-budget: FAIL', f)
print('vp-instruction-budget: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
