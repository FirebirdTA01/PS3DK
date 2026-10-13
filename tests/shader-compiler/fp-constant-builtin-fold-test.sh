#!/usr/bin/env bash
# Fragment literal builtins fold to constants, bit for bit as the reference
# folds them: rsqrt / exp2 / log2 / frac (the double-precision result rounded
# once to binary32) and dot / length / normalize / lerp (the measured
# binary32 orders).  Each row's expected const block is the reference's
# (sce-cgc 475, `return a * <expr>;`), measured on 64 random lanes per
# builtin in this profile; the folded program is one MUL reading the input
# and the constant.  Edge rows - rsqrt and log2 of x <= 0, an exp2 that
# overflows - stay run-time instructions, as the reference leaves them.
# exp / log / log10 fold the way their run-time lowerings compute: exp is
# the exp2 fold of f32(x * 0x3fb8aa3a) (log2(e) one ulp low, as the
# reference's multiply has it), log and log10 are f32(log2 fold * 0x3f317218
# / 0x3e9a209b); an exp whose exp2 overflows keeps a run-time EX2, and log
# of x <= 0 keeps its LG2.  sqrt / sin / cos / tan fold with the vertex
# rules; sqrt of x <= 0 stays at run time.  A literal vector divisor is one
# MUL by the per-lane reciprocals.  max / min / clamp / saturate / floor /
# ceil / sign / round / step / smoothstep / fmod of literals fold too.
#
# Usage: tests/shader-compiler/fp-constant-builtin-fold-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fp-constant-builtin-fold: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval as ev
import fp_sources as fs

FOLDED = [
    ('rsqrt0', 'rsqrt(float4(86.24646, 41.5752144, 2.85485053, 39.0324783))', '3ddc8698 3e1ecfd1 3f17832f 3e23e72b'),
    ('rsqrt1', 'rsqrt(float4(32.1056519, 18.4991703, 49.3701286, 81.369194))', '3e34b8a4 3e6e149c 3e11bc80 3de309ea'),
    ('exp21', 'exp2(float4(12.6628628, -0.542665124, -11.3104525, -3.09123278))', '45caa6fd 3f2fbe91 39ce6f7f 3df04ff9'),
    ('exp22', 'exp2(float4(-10.2290869, 12.1077776, 13.2193832, 12.1172905))', '3a5a69a9 4589edb7 461505ad 458ad750'),
    ('log20', 'log2(float4(880.234924, 500.134094, 703.967896, 60.1943817))', '411c8207 410f7570 41175990 40bd2b79'),
    ('log21', 'log2(float4(307.000763, 389.469025, 942.663879, 425.702057))', '4104318e 4109af93 411e16f0 410bbd3c'),
    ('frac0', 'frac(float4(35.8750763, -13.1659651, 0.499495864, -52.2187576))', '3f600500 3f558350 3effbdec 3f47ff80'),
    ('frac1', 'frac(float4(-12.8674288, -75.7769623, -5.9343791, 50.0751419))', '3e07c0c0 3e646400 3d866440 3d99e400'),
    ('exp0', 'exp(float4(0.265944153, -16.4019413, 12.1587648, -13.7821245))', '3fa6ff2b 33a1ae43 483a499e 358ac64e'),
    ('exp1', 'exp(float4(-9.52731323, 21.4118423, 22.8888874, 12.1609449))', '3898bf0c 4eed54bb 5001f033 483ab1b3'),
    ('exp_tiny', 'exp(float4(-90.0, -104.0, 0.5, -87.5))', '0008ec2c 00000000 3fd3094c 006cb303'),
    ('log0', 'log(float4(0.0022271187, 0.0209956933, 0.155928969, 1.04494688e-06))', 'c0c36ced c0774292 bfedde91 c15c583f'),
    ('log1', 'log(float4(8889.55957, 691.108032, 0.052557528, 3.25337646e-06))', '41117b6c 40d139ba c03c88c2 c14a2c4f'),
    ('log100', 'log10(float4(60.3577805, 5.79703885e-09, 11.7771626, 0.0381473042))', '3fe3ef12 c103c9e8 3f8917dc bfb59298'),
    ('log101', 'log10(float4(748.380188, 133776920, 19.5759239, 135030.703))', '4037f19f 410205a9 3fa55728 40a42c82'),
    # sqrt / sin / cos / tan: the vertex rules (sqrt = 1 / f32(1 / sqrt), tan = f32(sin) / f32(cos)), any finite trig argument.
    ('sqrt0', 'sqrt(float4(22.9411163, 44.5651627, 36.8661385, 26.9908123))', '40994521 40d59f73 40c24bc5 40a63fa3'),
    ('sin0', 'sin(float4(-3.27712703, 5.04632711, 8.45390129, 8.24506569))', '3e0a5cd9 bf71dbb8 3f534c2b 3f6cabcb'),
    ('cos0', 'cos(float4(2.44486666, -2.20816875, -4.22067118, 9.86312771))', 'bf4456a8 bf18578b bef1bc73 bf67cbca'),
    ('tan0', 'tan(float4(8.58440399, 3.83918285, 7.76364946, -7.92835331))', 'bf8ee88f 3f5692ca 4130a45b 4156bd27'),
    ('sinbig', 'sin(float4(100000.0, -70000.0, 3.0, 0.5))', '3d126d55 3f52cf32 3e1081c3 3ef57744'),
    ('d0', 'dot(float4(-5.41331816, -1.08807659, -2.62803507, -4.60329819), float4(-3.27712703, 5.04632711, 8.45390129, 8.24506569))', 'c23fb070 00000000 00000000 00000000'),
    ('d1', 'dot(float4(2.44486666, -2.20816875, -4.22067118, 9.86312771), float4(8.58440399, 3.83918285, 7.76364946, -7.92835331))', 'c2c4e97b 00000000 00000000 00000000'),
    ('l0', 'length(float3(4.81628847, 8.21494389, 1.00342691))', '411934f3 00000000 00000000 00000000'),
    ('l1', 'length(float3(2.41578174, 8.42691708, 4.06512547))', '411a9bc7 00000000 00000000 00000000'),
    ('n0', 'float4(normalize(float3(4.37469339, 8.7246666, 8.43615723)), 1)', '3ead9f66 3f2d21d1 3f27682c 3f800000'),
    ('p0', 'lerp(float4(2.17020774, -2.99076414, 0.790844798, 9.13028336), float4(-0.104225881, 5.08994484, -5.22051954, 8.24076366), float4(0.78861326, 0.833548129, 0.500092566, 0.900380611))', '3ec0cc60 406fac60 c00dc904 41054521'),
]
# Run-time edges: the opcode that must remain.
EDGES = [('rsqrt_zero', 'rsqrt(0.0)', {'RSQ', 'DIVSQR', 'DIVSQ'}),
         ('log2_negative', 'log2(-1.0)', {'LG2'}),
         ('exp2_overflow', 'exp2(200.0)', {'EX2'}),
         ('exp_overflow', 'exp(89.0)', {'EX2'}),
         ('log_zero', 'log(0.0)', {'LG2'}),
         ('log10_negative', 'log10(-2.0)', {'LG2'}),
         ('sqrt_zero', 'sqrt(0.0)', {'RSQ', 'DIVSQR', 'DIVSQ'}),
         ('sqrt_negative', 'sqrt(-1.0)', {'RSQ', 'DIVSQR', 'DIVSQ'})]
# A literal vector divisor is one MUL by the per-lane reciprocals (the
# reference's const block; a / k for a scalar k already was).
DIVISORS = [('div0', 'float4(2.0, 3.0, 4.0, 5.0)', '3f000000 3eaaaaab 3e800000 3e4ccccd'),
            ('div1', 'float4(-0.7, 11.0, 0.1, 1e-3)', 'bfb6db6e 3dba2e8c 41200000 4479ffff')]
fails = 0
def compile_(name, expr, body='return a * %s;'):
    cg = work / (name + '.cg'); out = work / (name + '.fpo')
    cg.write_text('float4 main(float4 a : TEXCOORD0) : COLOR { ' + body % expr + ' }' + chr(10))
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(out), str(cg)], capture_output=True, text=True)
    if p.returncode != 0 or not out.exists():
        return None, (p.stderr or '').strip()[:160]
    return list(fs.instructions(fs.ucode_words(out.read_bytes()))), ''
for name, expr, want in FOLDED:
    ins, err = compile_(name, expr)
    if ins is None:
        fails += 1; print('fp-constant-builtin-fold: FAIL %s: compile: %s' % (name, err)); continue
    ops = [fs.render(w, i).split()[1] for i, (w, c) in enumerate(ins)]
    blocks = [' '.join(struct.pack('>f', x).hex() for x in ev._const_floats(c)) for w, c in ins if c is not None]
    ok = ops == ['MUL'] and blocks == [want]
    fails += not ok
    print('fp-constant-builtin-fold: %s %s %s%s' % ('ok  ' if ok else 'FAIL', name, ' '.join(ops),
          '' if ok else '  const %s, want %s' % (blocks, want)))
for name, expr, keep in EDGES:
    ins, err = compile_(name, expr)
    if ins is None:
        fails += 1; print('fp-constant-builtin-fold: FAIL %s: compile: %s' % (name, err)); continue
    ops = {fs.render(w, i).split()[1] for i, (w, c) in enumerate(ins)}
    ok = bool(ops & keep)
    fails += not ok
    print('fp-constant-builtin-fold: %s %s keeps a run-time %s (%s)' % ('ok  ' if ok else 'FAIL', name, '/'.join(sorted(keep)), ' '.join(sorted(ops))))
# Component-wise builtins of literals: one instruction, and the program
# with a = 1 gives the reference's values (measured as a * (expr + 1.0), so
# no folded lane is zero and every lane is written).
COMPWISE = [
    ('abs0', 'abs(float4(-0.333333343, 2.5, -7.25, 0.125)) + 2.0', '40555555 40b00000 41240000 40480000'),
    ('max0', 'max(float4(3.62450814, -0.843062758, -4.71548653, -1.09736204), float4(-1.79011369, -3.15089798, -0.0634935498, 3.13673329))', '4093fbf8 3e20b428 3f6fbee3 4084601e'),
    ('min0', 'min(float4(-3.42618155, 0.737915933, 1.71920955, -3.80099869), float4(-3.7322309, -3.57927394, -1.63105237, -3.28558993))', 'c02edcdf c02512d3 bf218ca6 c0334390'),
    ('clamp0', 'clamp(float4(-2.62273622, 1.88550305, 1.68355668, -2.66292644), -1.0, 2.0)', '00000000 4038ac15 402bbf64 00000000'),
    ('saturate0', 'saturate(float4(-0.217722952, 1.65065837, -0.654165387, -0.724127889))', '3f800000 40000000 3f800000 3f800000'),
    ('floor0', 'floor(float4(-5.97182274, -2.94785714, -2.60005021, -1.89849973))', 'c0a00000 c0000000 c0000000 bf800000'),
    ('ceil0', 'ceil(float4(-3.22520542, -3.47325563, -5.98040771, 1.82384026))', 'c0000000 c0000000 c0800000 40400000'),
    ('sign0', 'sign(float4(-3.5, 0.0, 2.25, -0.0))', '00000000 3f800000 40000000 3f800000'),
    ('round0', 'round(float4(2.5, -2.5, 0.49999997, -7.3))', '40800000 bf800000 40000000 c0c00000'),
    ('step0', 'step(float4(-0.0604715273, -0.23739779, 1.23804855, 0.894264579), float4(-0.0870666727, -1.331815, 0.445085645, 1.64311647))', '3f800000 3f800000 3f800000 40000000'),
    ('smooth0', 'smoothstep(float4(-1.0388875, -0.161248475, -0.23385489, -0.545390129), float4(2.56731343, 2.16478968, 1.66606033, 1.07205665), float4(-0.879646122, 1.10299599, 0.0744238645, 2.33270097))', '3f80ba0a 3fc8559d 3f89042f 40000000'),
    ('fmod0', 'fmod(float4(-22.2809277, -20.1464653, 17.1287918, 2.2991128), float4(3.0, -2.5, 0.7, -6.1))', 'be8fd5b0 3f5a8140 3faa15e0 405324aa'),
]
for name, expr, want in COMPWISE:
    ins, err = compile_(name, expr, 'return a * (%s + 1.0);')
    if ins is None:
        fails += 1; print('fp-constant-builtin-fold: FAIL %s: compile: %s' % (name, err)); continue
    ops = [fs.render(w, i).split()[1] for i, (w, c) in enumerate(ins)]
    blob = (work / (name + '.fpo')).read_bytes()
    try:
        got = ' '.join(struct.pack('>f', x).hex() for x in ev.evaluate(blob, {'TEX0': [1.0] * 4}))
    except Exception as e:  # noqa: BLE001
        got = 'unmodelled: %s' % e
    ok = len(ops) == 1 and got == want
    fails += not ok
    print('fp-constant-builtin-fold: %s %s %s%s' % ('ok  ' if ok else 'FAIL', name, ' '.join(ops),
          '' if ok else '  values %s, want %s' % (got, want)))
for name, div, want in DIVISORS:
    ins, err = compile_(name, div, 'return a / %s;')
    if ins is None:
        fails += 1; print('fp-constant-builtin-fold: FAIL %s: compile: %s' % (name, err)); continue
    ops = [fs.render(w, i).split()[1] for i, (w, c) in enumerate(ins)]
    blocks = [' '.join(struct.pack('>f', x).hex() for x in ev._const_floats(c)) for w, c in ins if c is not None]
    ok = ops == ['MUL'] and blocks == [want]
    fails += not ok
    print('fp-constant-builtin-fold: %s %s a / %s: %s%s' % ('ok  ' if ok else 'FAIL', name, div, ' '.join(ops),
          '' if ok else '  const %s, want %s' % (blocks, want)))
if fails:
    print('fp-constant-builtin-fold: %d failure(s)' % fails); sys.exit(1)
print('PASS: fp-constant-builtin-fold-test')
PY
