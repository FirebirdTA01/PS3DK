#!/usr/bin/env bash
# Fragment modifier copies and direct output writes.  abs() and negation
# are NV40 source modifiers: a temp copied through `MOV d, |s|` or
# `MOV d, -s` is read through its source by its arithmetic readers and the
# copy is dropped.  The instruction that computes the colour writes the
# output register itself instead of a temp followed by `MOV R0, t`.
# Each row's instruction count is held to a budget and reported beside the
# reference's (sce-cgc 475); every arithmetic row is judged by value against
# independent binary32 arithmetic with fp_eval.
#
# A texture fetch keeps its coordinate copy (tex_keep): fp_eval does not
# model fetches, and the fold is limited to arithmetic readers.  The
# reference reads |TEX0| in the fetch itself (1 instruction), and folds a
# negated input register into its reader (neg_mad, 3) - both are later work.
#
# Usage: tests/shader-compiler/fp-modifier-output-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fp-modifier-output: FAIL: compiler required"; exit 1; }
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
MOV = 0x01

# name: (source, budget, reference count, expected(a, b) or None)
ROWS = {
    'abs_mul': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return a * abs(b.x); }',
                2, 3, lambda a, b: [f32(x * abs(b[0])) for x in a]),
    'neg_mad': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float4 n = -b; return a * n.y + b; }',
                4, 3, lambda a, b: [f32(f32(a[i] * -b[1]) + b[i]) for i in range(4)]),
    'abs_neg': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float t = abs(b.z); return a - (-t) * b; }',
                3, 3, lambda a, b: [f32(a[i] - f32(-abs(b[2]) * b[i])) for i in range(4)]),
    'out_direct': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { float3 c = a.xyz * b.xyz; return c.xyzz; }',
                   3, 3, lambda a, b: [f32(a[0] * b[0]), f32(a[1] * b[1]), f32(a[2] * b[2]), f32(a[2] * b[2])]),
    'tex_keep': ('float4 main(float4 a : TEXCOORD0, uniform sampler2D s : TEXUNIT0) : COLOR { return tex2D(s, abs(a.xy)); }',
                 3, 1, None),
}

fails = 0
def fail(msg):
    global fails
    fails += 1
    print('fp-modifier-output: FAIL ' + msg)

rng = random.Random(20261012)
for name, (src, budget, ref, expect) in ROWS.items():
    cg = work / (name + '.cg'); out = work / (name + '.fpo')
    cg.write_text(src + chr(10))
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out), str(cg)],
                       capture_output=True, text=True)
    if p.returncode != 0 or not out.exists():
        fail('%s: compile rc %d: %s' % (name, p.returncode, (p.stderr or '').strip()[:200]))
        continue
    blob = out.read_bytes()
    ins = [w for w, c in fs.instructions(fs.ucode_words(blob))]
    n = len(ins)
    last = ins[-1]
    last_op = (last[0] >> 24) & 0x3F
    shape_ok = n <= budget
    if name != 'tex_keep':
        text = [fs.render(w, i) for i, w in enumerate(ins)]
        if any(t.split()[1] == 'MOV' and ('s0=|R' in t or 's0=-R' in t or 's0=-|R' in t) for t in text):
            fail('%s: a modified temp copy survived: %s' % (name, ' | '.join(text)))
            shape_ok = False
        if last_op == MOV:
            fail('%s: the colour is written by a trailing MOV: %s' % (name, text[-1]))
            shape_ok = False
    if n > budget:
        fail('%s: %d instructions, budget %d (reference %d)' % (name, n, budget, ref))
    if expect is not None:
        judged = 0
        for _ in range(16):
            a = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            b = [f32(rng.uniform(-2, 2)) for _ in range(4)]
            try:
                got = ev.evaluate(blob, {'TEX0': a, 'TEX1': b})
            except ev.Unmodelled as e:
                fail('%s: unmodelled: %s' % (name, e)); break
            want = expect(a, b)
            if struct.pack('>4f', *got) != struct.pack('>4f', *want):
                fail('%s: value %r, want %r (a=%r b=%r)' % (name, got, want, a, b)); break
            judged += 1
        print('fp-modifier-output: %s %s %d instructions (budget %d, reference %d), %d values' %
              ('ok  ' if shape_ok and judged == 16 else 'FAIL', name, n, budget, ref, judged))
    else:
        print('fp-modifier-output: %s %s %d instructions (budget %d, reference %d), shape only' %
              ('ok  ' if shape_ok else 'FAIL', name, n, budget, ref))

# Control: the value judge rejects a wrong result.
blob = (work / 'abs_mul.fpo').read_bytes()
a, b = [1.0, -2.0, 0.5, 3.0], [-1.5, 0.0, 0.0, 0.0]
wrong = [f32(x * b[0]) for x in a]   # abs() dropped
if struct.pack('>4f', *ev.evaluate(blob, {'TEX0': a, 'TEX1': b})) == struct.pack('>4f', *wrong):
    fail('control: the abs-dropped value was accepted')
else:
    print('fp-modifier-output: ok   control: an abs-dropped value is rejected')

if fails:
    print('fp-modifier-output: %d failure(s)' % fails)
    sys.exit(1)
print('PASS: fp-modifier-output-test')
PY
