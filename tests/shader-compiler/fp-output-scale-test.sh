#!/usr/bin/env bash
# Fragment output scale: a product of one arithmetic result with a
# power-of-two literal becomes that result's output scale, as the reference
# emits it (sce-cgc 475, measured): x2 / x4 / x8 / /4 fold into MUL, ADD,
# MAD and DP3, and (t * 2) * 2 is one x4.  A factor of 3, a fetch result and
# a saturated producer keep their multiply.  Each row pins the last
# instruction's opcode and scale code and is judged by value against
# independent binary32 arithmetic with fp_eval (which models the scale).
#
# Usage: tests/shader-compiler/fp-output-scale-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fp-output-scale: FAIL: compiler required"; exit 1; }
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
HEAD = 'float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1, float4 c : TEXCOORD2, uniform sampler2D s : TEXUNIT0) : COLOR { '
# name: (body, last opcode, scale code of the last instruction, expected(a, b, c) or None)
ROWS = {
    'mul_x2':  ('float4 t = a * b; return t * 2.0; }', 'MUL', 1, lambda a, b, c: [f32(f32(x * y) * 2) for x, y in zip(a, b)]),
    'mul_x4':  ('float4 t = a * b; return t * 4.0; }', 'MUL', 2, lambda a, b, c: [f32(f32(x * y) * 4) for x, y in zip(a, b)]),
    'mul_x8':  ('float4 t = a * b; return t * 8.0; }', 'MUL', 3, lambda a, b, c: [f32(f32(x * y) * 8) for x, y in zip(a, b)]),
    'mul_q':   ('float4 t = a * b; return t * 0.25; }', 'MUL', 6, lambda a, b, c: [f32(f32(x * y) / 4) for x, y in zip(a, b)]),
    'add_x2':  ('float4 t = a + b; return t * 2.0; }', 'ADD', 1, lambda a, b, c: [f32(f32(x + y) * 2) for x, y in zip(a, b)]),
    'mad_x2':  ('float4 t = a * b + c; return t * 2.0; }', 'MAD', 1, None),
    'dp3_x2':  ('float t = dot(a.xyz, b.xyz); return t.xxxx * 2.0; }', 'DP3', 1, None),
    'twice':   ('float4 t = a * b; return (t * 2.0) * 2.0; }', 'MUL', 2, lambda a, b, c: [f32(f32(x * y) * 4) for x, y in zip(a, b)]),
    # Not folded, as in the reference.
    'mul_x3':  ('float4 t = a * b; return t * 3.0; }', 'MUL', 0, lambda a, b, c: [f32(f32(x * y) * 3) for x, y in zip(a, b)]),
    'tex_x2':  ('float4 t = tex2D(s, a.xy); return t * 2.0; }', 'MUL', 0, None),
    'sat_x2':  ('float4 t = saturate(a * b); return t * 2.0; }', None, 0, lambda a, b, c: [f32(min(1.0, max(0.0, f32(x * y))) * 2) for x, y in zip(a, b)]),
}
fails = 0
rng = random.Random(1013)
for name, (body, last_op, want_scale, expect) in ROWS.items():
    cg = work / (name + '.cg'); out = work / (name + '.fpo')
    cg.write_text(HEAD + body + chr(10))
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out), str(cg)], capture_output=True, text=True)
    if p.returncode != 0 or not out.exists():
        fails += 1; print('fp-output-scale: FAIL %s: compile rc %d' % (name, p.returncode)); continue
    blob = out.read_bytes()
    ins = [w for w, c in fs.instructions(fs.ucode_words(blob))]
    ops = [fs.render(w, i).split()[1] for i, w in enumerate(ins)]
    scales = [(w[2] >> 28) & 7 for w in ins]
    ok = (last_op is None or ops[-1] == last_op) and scales[-1] == want_scale
    if want_scale == 0:
        ok = ok and not any(scales)        # nothing anywhere picked up a scale
    judged = 0
    if expect is not None and ok:
        for _ in range(12):
            a = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            b = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            c = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            got = ev.evaluate(blob, {'TEX0': a, 'TEX1': b, 'TEX2': c})
            if struct.pack('>4f', *got) != struct.pack('>4f', *expect(a, b, c)):
                ok = False; print('    %s: got %r want %r' % (name, got, expect(a, b, c))); break
            judged += 1
    fails += not ok
    print('fp-output-scale: %s %s %s (scale %d)%s' % ('ok  ' if ok else 'FAIL', name, ' '.join(ops), scales[-1],
          ', %d values' % judged if judged else ''))
if fails:
    print('fp-output-scale: %d failure(s)' % fails); sys.exit(1)
print('PASS: fp-output-scale-test')
PY
