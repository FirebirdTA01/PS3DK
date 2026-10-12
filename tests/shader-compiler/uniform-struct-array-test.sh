#!/usr/bin/env bash
# A uniform ARRAY of structs read by a constant index (PSGL's vpFixedPipeline:
# `uniform TexUnit texunit[4]; ... mul(texunit[0].matrix, otc0)`).  Measured
# on the reference: every element member is a uniform record of its own,
# "s[2].m", listed element-major in field order, with no record for "s" or
# "s[k]", and each is allocated like an ordinary uniform (an unread member
# takes no register).  Judged by records, registers and value; a run-time
# index is refused by name until it is lowered.
#
# Usage: tests/shader-compiler/uniform-struct-array-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "uniform-struct-array: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import vp_pow_vector_check as vp_eval
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
NL = chr(10)
fails = []

def compile_one(name, src, profile='sce_vp_rsx'):
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src + NL)
    p = subprocess.run([compiler, '-p', profile, '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=60)
    return p, o

def records(b):
    n, t = struct.unpack_from('>2I', b, 12); out = []
    for i in range(n):
        x = struct.unpack_from('>11I', b, t + 48 * i)
        if x[2] == 0x1006:   # uniform: (name, register, isReferenced)
            out.append((b[x[4]:b.index(0, x[4])].decode(), x[3], x[10]))
    return out

SRC = ('struct S { float4 a; float4x4 m; float b; };' + NL + 'uniform S s[3];' + NL +
       'float4 main(float4 p : POSITION) : POSITION { return mul(s[2].m, p) + s[1].a * s[0].b; }')
UNREF = 0xFFFFFFFF
# (name, register, isReferenced) as the reference lists them, in its order.
WANT = ([('s[0].a', UNREF, 0), ('s[0].m', UNREF, 0)] + [('s[0].m[%d]' % r, UNREF, 0) for r in range(4)] +
        [('s[0].b', 467, 1), ('s[1].a', 466, 1), ('s[1].m', UNREF, 0)] +
        [('s[1].m[%d]' % r, UNREF, 0) for r in range(4)] + [('s[1].b', UNREF, 0), ('s[2].a', UNREF, 0)] +
        [('s[2].m', 256, 1)] + [('s[2].m[%d]' % r, 256 + r, 1) for r in range(4)] + [('s[2].b', UNREF, 0)])
p, o = compile_one('vp', SRC)
if p.returncode != 0 or not o.exists():
    fails.append('vp: refused (rc %d): %s' % (p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
else:
    blob = o.read_bytes()
    got = [r for r in records(blob) if r[0].startswith('s')]
    if [r[0] for r in got] != [w[0] for w in WANT]:
        fails.append('record order: got %s' % [r[0] for r in got])
    if sorted(got) != sorted(WANT):
        fails.append('records: got %s want %s' % (got, WANT))
    print('  %-28s %s' % ('records', 'ok' if sorted(got) == sorted(WANT) else 'WRONG'))
    rows = [[1.0, 2.0, -0.5, 0.25], [0.5, -1.0, 3.0, 2.0], [-2.0, 0.75, 1.0, -1.5], [0.125, 4.0, -3.0, 1.0]]
    uni = {'s[2].m[%d]' % r: rows[r] for r in range(4)}
    uni.update({'s[1].a': [1.5, -2.0, 0.5, 3.0], 's[0].b': [-0.75, 0, 0, 0],
                's[0].a': [99.0] * 4, 's[1].b': [99.0] * 4, 's[2].a': [99.0] * 4})
    wrong = 0
    for pos in ([0.5, -1.0, 2.0, 1.0], [-3.0, 0.25, 1.5, -2.0]):
        pos = [f32(x) for x in pos]
        out = vp_eval.evaluate(blob, uni, inputs={0: pos}, binary32=True)
        want = [f32(f32(sum(f32(rows[r][c] * pos[c]) for c in range(4))) + f32(uni['s[1].a'][r] * -0.75))
                for r in range(4)]
        if any(abs(a - b) > 1e-5 * max(1.0, abs(b)) for a, b in zip(out.get(0, [None] * 4), want)):
            fails.append('value at %s: got %s want %s' % (pos, out.get(0), want)); wrong += 1
    print('  %-28s %s' % ('value', 'ok' if not wrong else '%d WRONG' % wrong))

# The fragment profile: the same per-element records, no "s" / "s[k]".
p, o = compile_one('fp', 'struct S { float4 a; float b; };' + NL + 'uniform S s[3];' + NL +
                   'float4 main(float4 t : TEXCOORD0) : COLOR { return s[2].a * t + s[0].b; }', 'sce_fp_rsx')
if p.returncode != 0 or not o.exists():
    fails.append('fp: refused (rc %d)' % p.returncode); print('  %-28s REFUSED' % 'fp')
else:
    names = [(r[0], r[2]) for r in records(o.read_bytes()) if r[0].startswith('s')]
    want = [('s[%d].%s' % (k, m), int((k, m) in ((0, 'b'), (2, 'a')))) for k in range(3) for m in 'ab']
    if names != want: fails.append('fp records: got %s want %s' % (names, want))
    print('  %-28s %s' % ('fp records', 'ok' if names == want else 'WRONG'))

# A sampler member's binding (`sampler2D tex : TEXUNIT3`) carries to every
# element, read or not, and the TEX samples unit 3; the same for a plain
# uniform struct (codex's probe; both used to sample unit 0).
def sampler_info(b):
    h = struct.unpack_from('>8I', b, 0); recs = []
    for i in range(h[3]):
        x = struct.unpack_from('>11I', b, h[4] + 48 * i)
        nm = b[x[4]:b.index(0, x[4])].decode()
        if nm.startswith('s'):
            recs.append((nm, x[1], b[x[7]:b.index(0, x[7])].decode() if x[7] else '', x[10]))
    w = struct.unpack_from('>%dI' % (h[6] // 4), b, h[7])
    return recs, [(w[i] >> 1) & 0xf for i in range(0, len(w), 4) if (w[i] & 0xff00) == 0x1700]
for name, src, want in [
    ('sampler_member_array', 'struct S { sampler2D tex : TEXUNIT3; };' + NL + 'uniform S s[2];' + NL +
     'float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(s[1].tex, uv); }',
     ([('s[0].tex', 0x803, 'TEXUNIT3', 0), ('s[1].tex', 0x803, 'TEXUNIT3', 1)], [3])),
    ('sampler_member_struct', 'struct S { sampler2D tex : TEXUNIT3; };' + NL + 'uniform S s;' + NL +
     'float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(s.tex, uv); }',
     ([('s.tex', 0x803, 'TEXUNIT3', 1)], [3])),
]:
    p, o = compile_one(name, src, 'sce_fp_rsx')
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); print('  %-28s REFUSED' % name); continue
    got = sampler_info(o.read_bytes())
    if got != want: fails.append('%s: got %s want %s' % (name, got, want))
    print('  %-28s %s' % (name, 'ok' if got == want else 'WRONG'))

# A run-time index is refused by name, with no container.
p, o = compile_one('dyn', 'struct S { float4 a; };' + NL + 'uniform S s[4]; uniform float i;' + NL +
                   'float4 main(float4 p : POSITION) : POSITION { return s[int(i)].a + p; }')
ok = p.returncode != 0 and not o.exists() and 'struct-array-dynamic-index' in p.stdout + p.stderr
print('  %-28s %s' % ('run-time index refused', 'ok' if ok else 'NOT refused (rc %d)' % p.returncode))
if not ok: fails.append('run-time index: expected the struct-array-dynamic-index refusal')

for f in fails: print('uniform-struct-array: FAIL', f)
print('uniform-struct-array: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
