#!/usr/bin/env bash
# An if-statement whose condition the compiler reduces to a constant takes
# one arm at compile time, as the established compiler does: a comparison of static
# constants, and a loop index compared in an unrolled loop, leave no
# comparison instruction and no predicated other arm.  Each row pins the
# instruction list the established compiler's shape implies (the established compiler, measured) and
# is judged by value with fp_eval.  A comparison the fold must not touch (a
# uniform operand) keeps its run-time compare.
#
# Usage: tests/shader-compiler/fp-constant-branch-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fp-constant-branch: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import random, struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval as ev
import fp_sources as fs

f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
HEAD = ('static const float mode = 1.0;' + chr(10) + 'static const bool flag = false;' + chr(10) +
        'float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1, uniform float k) : COLOR { ')
COMPARES = {'SLT', 'SGT', 'SLE', 'SGE', 'SEQ', 'SNE'}
# name: (body, ops wanted (None: any), expected(a, b) or None, must keep a compare)
ROWS = {
    'static_gt':  ('float4 x; if (mode > 0.5) x = a * 2.0; else x = b * 3.0; return x; }', ['MUL'],
                   lambda a, b: [f32(x * 2) for x in a], False),
    'static_flag': ('float4 x = a; if (flag) x = sin(b); return x; }', ['MOV'], lambda a, b: list(a), False),
    'else_if':    ('float4 x = a; if (mode < 0.5) { x = b; } else if (mode < 1.5) { x = a + b; } else { x = a * b; } return x; }',
                   None, lambda a, b: [f32(x + y) for x, y in zip(a, b)], False),
    'loop_index': ('float4 x = a; for (int i = 0; i < 3; i++) { if (i == 1) x += b; } return x; }',
                   None, lambda a, b: [f32(x + y) for x, y in zip(a, b)], False),
    # Control: a uniform operand is a run-time comparison.
    'uniform':    ('float4 x = a; if (k > 0.5) x = b; return x; }', None, None, True),
}
fails = 0
rng = random.Random(1013)
for name, (body, want_ops, expect, keep_cmp) in ROWS.items():
    cg = work / (name + '.cg'); out = work / (name + '.fpo')
    cg.write_text(HEAD + body + chr(10))
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out), str(cg)], capture_output=True, text=True)
    if p.returncode != 0 or not out.exists():
        fails += 1; print('fp-constant-branch: FAIL %s: compile rc %d' % (name, p.returncode)); continue
    blob = out.read_bytes()
    ops = [fs.render(w, i).split()[1] for i, (w, c) in enumerate(fs.instructions(fs.ucode_words(blob)))]
    has_cmp = bool(COMPARES & set(ops))
    ok = has_cmp == keep_cmp and (want_ops is None or ops == want_ops)
    judged = 0
    if expect is not None and ok:
        for _ in range(12):
            a = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            b = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            got = ev.evaluate(blob, {'TEX0': a, 'TEX1': b})
            if struct.pack('>4f', *got) != struct.pack('>4f', *expect(a, b)):
                ok = False; print('    %s: got %r want %r' % (name, got, expect(a, b))); break
            judged += 1
    fails += not ok
    print('fp-constant-branch: %s %s %s%s' % ('ok  ' if ok else 'FAIL', name, ' '.join(ops),
          ', %d values' % judged if judged else ''))
if fails:
    print('fp-constant-branch: %d failure(s)' % fails); sys.exit(1)
print('PASS: fp-constant-branch-test')
PY
