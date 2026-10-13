#!/usr/bin/env bash
# A vertex program's rsqrt / exp2 / log2 / frac of a literal is folded to
# a constant, bit for bit what the reference folds (each expected word
# below was measured with sce-cgc 475: the double-precision result rounded
# once to binary32, subnormal results kept); so are dot (binary32 sum of
# the rounded products, left to right), length (the RSQ-then-RCP rule of
# sqrt), normalize (each lane times the rounded rsqrt) and lerp
# (a + t * (b - a), each step rounded).  rsqrt and log2 of x <= 0 and
# an exp2 that overflows are left to run time, as the reference leaves
# them.  exp / log / log10 fold as their lowerings compute (exp through
# log2(e) = 0x3fb8aa3a, one ulp below float32's; log and log10 as the
# log2 fold times 0x3f317218 / 0x3e9a209b); exp(89.0), log(0.0) and
# log10(-2.0) stay run time.  Ours emitted every one of these as run-time instructions.
#
# Usage: tests/shader-compiler/vp-constant-builtin-fold-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-constant-builtin-fold: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import vp_pow_vector_check as ev
NL = chr(10)
bits = lambda x: struct.unpack('>I', struct.pack('>f', x))[0]
# builtin -> [(argument, measured result word)]
FOLDED = {
    'rsqrt': [(4.714561939239502, 0x3eebcd90), (1.6779167652130127, 0x3f45a180), (9.67042064666748, 0x3ea4a4ff),
              (1.097411036491394, 0x3f745fca), (1.3538216352462769, 0x3f5c04be), (6.705321788787842, 0x3ec5b96f),
              (1e-40, 0x60ad790a), (3.9688870906829834, 0x3f008030)],
    'exp2': [(5.587489128112793, 0x42405628), (4.333008766174316, 0x41a13bce), (-4.84541130065918, 0x3d0e7a33),
             (-0.6543976068496704, 0x3f22a5b1), (1.2866324186325073, 0x401c2211), (-2.3893470764160156, 0x3e43731a),
             (-130.0, 0x00080000), (-200.0, 0x00000000)],
    'log2': [(1.0302661657333374, 0x3d3032b3), (7.622323513031006, 0x403b88e7), (5.308923721313477, 0x401a238b),
             (2.822493314743042, 0x3fbf9cb8), (8.397058486938477, 0x404478fb), (8.641921997070312, 0x40472065),
             (1e-40, 0xc304e08c), (3.5771007537841797, 0x3feb5d7f)],
    'frac': [(94.96155548095703, 0x3f762880), (56.31205368041992, 0x3e9fc580), (-10.163854598999023, 0x3f560da0),
             (-72.64484405517578, 0x3eb5d700), (1e9, 0x00000000), (-0.0, 0x00000000), (-1e-9, 0x3f800000),
             (10.122249603271484, 0x3dfa5e00)],
    # exp: the exp2 fold of f32(x * 0x3fb8aa3a); log / log10: f32(log2 fold * 0x3f317218 / 0x3e9a209b).
    'exp': [(-20.0424881, 0x3107bfe8), (-0.0932358503, 0x3f6935e5), (24.2850456, 0x510339e6), (-3.29307532, 0x3d181f8d),
            (-7.03010273, 0x3a67f4b9), (13.4517422, 0x4929b0b0), (1.24078441, 0x405d5532), (22.7928085, 0x4fec11cd)],
    'log': [(264707.375, 0x4147c836), (181.379669, 0x40a66b41), (4.93513198e-07, 0xc16858f4), (13.7861481, 0x4027ea1e),
            (46058432.0, 0x418d29d3), (0.148416862, 0xbff43082), (8.57174385e-08, 0xc1822d7c), (0.00629963819, 0xc0a22705)],
    'log10': [(4.1437989e-08, 0xc0ec3e46), (23271.9512, 0x408bbd19), (229188.734, 0x40ab86b5), (2.18350465e-07, 0xc0d525a6),
              (0.0313572884, 0xbfc077fb), (1.81445694, 0x3e847a72), (148157.734, 0x40a57694), (8.89253968e-08, 0xc0e1a195)],
}
# dot / length / normalize / lerp of literals, four lanes a compile.
EXPR_GROUPS = [
    [('dot(float3(1e8, 1, -1e8), float3(1, 1, 1))', 0x00000000),
     ('dot(float4(1.5, -2.25, 3.125, 0.1), float4(0.3, 0.7, -1.9, 4.4))', 0xc0d3eb85),
     ('dot(float2(12.7, -3.3), float2(0.11, 9.8))', 0xc1f78b44),
     ('length(float3(1, 2, 3))', 0x406f7751)],
    [('length(float4(0.3, -7.1, 2.2, 5.5))', 0x41140616),
     ('normalize(float3(1, 2, 3)).x', 0x3e88d677),
     ('normalize(float3(-4.4, 0.25, 9.1)).z', 0x3f666700),
     ('lerp(0.1, 0.7, 0.3)', 0x3e8f5c29)],
    [('lerp(-12.5, 33.3, 0.77)', 0x41b620c4),
     ('lerp(float3(1.1, 2.2, 3.3), float3(-5.5, 6.6, 0.01), 0.45).y', 0x4085c28f),
     ('length(float2(3e-3, 4e-3))', 0x3ba3d70a),
     ('dot(float3(0.1, 0.2, 0.3), float3(0.1, 0.2, 0.3))', 0x3e0f5c2a)],
]
RUNTIME = ['rsqrt(0.0)', 'rsqrt(-4.0)', 'log2(0.0)', 'log2(-1.0)', 'exp2(200.0)', 'exp(89.0)', 'log(0.0)', 'log10(-2.0)']

def compile_(name, exprs):
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text('float4 main(float4 p : POSITION) : POSITION { return float4(%s) + p * 0.0; }' % ', '.join(exprs) + NL)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0 or not o.exists():
        return None, None
    blob = o.read_bytes()
    return struct.unpack_from('>I', blob, 24)[0] // 16, blob

fails = []
for fn, cases in FOLDED.items():
    for g in range(0, len(cases), 4):
        group = cases[g:g + 4]
        count, blob = compile_('%s%d' % (fn, g), ['%s(%r)' % (fn, a) for a, _ in group])
        if blob is None:
            fails.append('%s group %d refused' % (fn, g)); continue
        got = ev.evaluate(blob, {}, inputs={0: [0.0] * 4}, binary32=True).get(0)
        words = [bits(x) for x in got] if got else []
        want = [w for _, w in group]
        if count != 1:
            fails.append('%s group %d: %d instructions, want the folded constant (1)' % (fn, g, count))
        if words != want:
            fails.append('%s group %d: %s, want %s' % (fn, g, ['%08x' % w for w in words], ['%08x' % w for w in want]))
        print('  %-6s %d..%d  %s' % (fn, g, g + len(group) - 1, 'ok' if count == 1 and words == want else 'FAIL'))
for g, group in enumerate(EXPR_GROUPS):
    count, blob = compile_('expr%d' % g, [e for e, _ in group])
    if blob is None:
        fails.append('expression group %d refused' % g); continue
    got = ev.evaluate(blob, {}, inputs={0: [0.0] * 4}, binary32=True).get(0)
    words = [bits(x) for x in got] if got else []
    want = [w for _, w in group]
    if count != 1:
        fails.append('expression group %d: %d instructions, want the folded constant (1)' % (g, count))
    if words != want:
        fails.append('expression group %d: %s, want %s' % (g, ['%08x' % w for w in words], ['%08x' % w for w in want]))
    print('  exprs  %d      %s' % (g, 'ok' if count == 1 and words == want else 'FAIL'))
for e in RUNTIME:
    count, blob = compile_('rt', [e, '0', '0', '0'])
    if blob is None or count < 2:
        fails.append('%s: folded or refused (%s instructions); the reference leaves it to run time' % (e, count))
    print('  %-12s %s' % (e, 'run time' if blob is not None and count >= 2 else 'FAIL'))
for f in fails: print('vp-constant-builtin-fold: FAIL', f)
print('vp-constant-builtin-fold: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
