#!/usr/bin/env bash
# A fragment entry's `out` parameter written under a branch (the SDK
# strip_branch fs_multi: `color = ...` in every arm of nested ifs, then
# `color.w = ...`): the parameter is joined at every merge like a local, and
# its final value is stored once at the end of the body.  It used to store in
# each arm, and the flatten refused an output store a path can skip.
# The same for a vertex program's out parameter.  Judged by value; a body
# with a discard or a return keeps its old path.
#
# Usage: tests/shader-compiler/out-param-branch-store-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "out-param-branch-store: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval
import vp_pow_vector_check as vp_eval
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
NL = chr(10)
M = 'void main(float4 t : TEXCOORD0, out float4 c : COLOR) { '
def nested(t):
    if t[0] > 0:
        c = [t[0], t[1], t[2], t[3]] if t[1] > 0 else [t[1], t[0], t[3], t[2]]
    else:
        c = [f32(t[2] * 2.0), 0.0, 1.0, 0.5]
    c[3] = 0.25
    return c
VALUE = [
    ('if_else', M + 'if (t.x > 0) c = t; else c = t.yxwz; }',
     lambda t: list(t) if t[0] > 0 else [t[1], t[0], t[3], t[2]]),
    ('nested_then_lane', M + 'if (t.x > 0) { if (t.y > 0) c = t; else c = t.yxwz; } '
                         'else c = float4(t.z * 2.0, 0, 1, 0.5); c.w = 0.25; }', nested),
    ('else_if_chain', M + 'if (t.x > 1) c = float4(1, 0, 0, 1); else if (t.x > 0) c = float4(0, 1, 0, 1); '
                      'else if (t.y > 0) c = float4(0, 0, 1, 1); else c = float4(t.w, t.w, t.w, 1); }',
     lambda t: [1.0, 0.0, 0.0, 1.0] if t[0] > 1 else [0.0, 1.0, 0.0, 1.0] if t[0] > 0
               else [0.0, 0.0, 1.0, 1.0] if t[1] > 0 else [t[3], t[3], t[3], 1.0]),
    ('written_then_overwritten_in_arm', M + 'c = t; if (t.x > 0) c = t * 2.0; }',
     lambda t: [f32(x * 2.0) for x in t] if t[0] > 0 else list(t)),
]
TS = [[0.5, 0.25, -1.5, 2.0], [0.5, -0.25, 1.5, -2.0], [-1.0, 0.75, 0.125, 3.0],
      [2.0, 1.0, 1.0, 1.0], [-1.0, -0.5, 0.5, 0.75]]
fails = []
for name, src, want_fn in VALUE:
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src + NL)
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=60)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); print('  %-32s REFUSED' % name); continue
    wrong = 0
    for t in TS:
        t = [f32(x) for x in t]
        got = fp_eval.evaluate(o.read_bytes(), {'TEX0': t}); want = [f32(x) for x in want_fn(t)]
        if got != want:
            fails.append('%s at t=%s: got %s want %s' % (name, t, got, want)); wrong += 1
    print('  %-32s %s' % (name, 'ok' if not wrong else '%d WRONG' % wrong))
# The same in a vertex program (the SDK TangentNormalMapVp:
# `if (tangent.w) oNormal *= -1;`).
VP = ('void main(float4 p : POSITION, float4 t : TEXCOORD0, out float4 op : POSITION, out float4 o : TEXCOORD0) '
      '{ op = p; o = t; if (t.w > 0) o = o * -1.0; }')
s, o = work / 'vp.cg', work / 'vp.bin'
s.write_text(VP + NL)
p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                   capture_output=True, text=True, timeout=60)
if p.returncode != 0 or not o.exists():
    fails.append('vertex_if: refused (rc %d)' % p.returncode); print('  %-32s REFUSED' % 'vertex_if')
else:
    wrong = 0
    for t in TS:
        t = [f32(x) for x in t]
        pos = [.25, -.5, .75, 1.]
        out = vp_eval.evaluate(o.read_bytes(), {}, inputs={0: pos, 8: t}, binary32=True, predication=True)
        want = [f32(-x) for x in t] if t[3] > 0 else list(t)
        if out.get(0) != pos or out.get(7) != want:
            fails.append('vertex_if at t=%s: got %s want %s' % (t, out.get(7), want)); wrong += 1
    print('  %-32s %s' % ('vertex_if', 'ok' if not wrong else '%d WRONG' % wrong))
for f in fails: print('out-param-branch-store: FAIL', f)
print('out-param-branch-store: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
