"""pow() with a CONSTANT exponent (t_0f3b232e, Boy_HairFp).

Reference (sce-cgc 475, measured 2026-09-15, .local/probe-boyhair, 30 cells):
a constant exponent is not always the LG2 / MUL / EX2 chain.  In FP:
0 -> the constant 1; 1 -> a copy; 2 -> MUL x, x (vectors too); 3 -> MUL, MUL;
-1 -> RCP; -0.5 -> RSQ; 0.5 -> DIVSQR |x|, x; 4 / 8 / 0.25 / 0.125 -> LG2 with
the multiply folded into the output scale (M4 M8 D4 D8) then EX2; -2 / -4 /
-8 / -0.25 the same with EX2 reading the NEGATED log; everything else (5, 6,
16, 32, 1.5, 0.75, -3, a variable) is LG2, MUL, EX2.  In VP: 2 -> MUL,
3 -> MUL MUL, -1 -> RCP, 0.5 -> RSQ + RCP, others the chain.

The first rows carry VALUE: LG2 of a NEGATIVE base is NaN, so the old
uniform chain painted NaN wherever pow(dot(t, h), 2) had a negative dot
(Boy_HairFp, 1369 of 4096 pixels).  Every value row here is executed on a
negative-base input with an independent python expectation; the scale rows
decode the LG2 scale bits and the EX2 source sign; the twin rows compare
bytes against the spelling the reference emits identically.

Usage: pow_constant_check.py <workdir>
"""
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from fp_sources import ARITY, CONST, INPUT, TEMP, instructions, source, ucode_words  # noqa: E402
from vp_words import decode  # noqa: E402

failures = []


def require(ok, why):
    if not ok:
        failures.append(why)


def f32(v):
    return struct.unpack('>f', struct.pack('>f', v))[0]


SCALE = {0: 1.0, 1: 2.0, 2: 4.0, 3: 8.0, 5: 0.5, 6: 0.25, 7: 0.125}
OP_MOV, OP_MUL, OP_ADD, OP_MAD, OP_DP3, OP_DP4 = 1, 2, 3, 4, 5, 6
OP_RCP, OP_RSQ, OP_EX2, OP_LG2, OP_DIVSQR = 0x1A, 0x1B, 0x1C, 0x1D, 0x3B


def decode_fp(blob):
    """Yield (opcode, dst, mask, scale, args[lanes], srcs) per fragment instruction."""
    out = []
    for w, const in instructions(ucode_words(blob)):
        ended = bool(w[0] & 1)
        op = (w[0] >> 24) & 63
        if op in (0, 0x3e):
            if ended:
                break
            continue
        srcs = [source(w, slot) for slot in range(1, ARITY.get(op, 0) + 1)]
        scale = (w[2] >> 28) & 0xF
        out.append({'op': op, 'dst': (w[0] >> 1) & 63, 'mask': (w[0] >> 9) & 15,
                    'scale': scale, 'srcs': srcs, 'const': const, 'end': ended})
        if ended:
            break
    return out


def execute_fp(blob, vector):
    regs = {}
    for ins in decode_fp(blob):
        op = ins['op']
        args = []
        for s in ins['srcs']:
            if s['type'] == INPUT:
                if s['name'] != 'TEX0':
                    raise AssertionError('unexpected input ' + str(s['name']))
                data = vector
            elif s['type'] == CONST:
                if ins['const'] is None:
                    raise AssertionError('missing inline block')
                data = [struct.unpack('>f', struct.pack('>I', x))[0] for x in ins['const']]
            else:
                if not (s['type'] == TEMP and s['reg'] in regs):
                    raise AssertionError('uninitialized temporary')
                data = regs[s['reg']]
            lanes = [data[(s['swizzle'] >> (2 * i)) & 3] for i in range(4)]
            if s['abs']:
                lanes = [abs(v) for v in lanes]
            if s['negate']:
                lanes = [-v for v in lanes]
            args.append(lanes)
        if op == OP_MOV:
            result = args[0]
        elif op == OP_MUL:
            result = [f32(a * b) for a, b in zip(args[0], args[1])]
        elif op == OP_ADD:
            result = [f32(a + b) for a, b in zip(args[0], args[1])]
        elif op == OP_MAD:
            result = [f32(f32(a * b) + c) for a, b, c in zip(args[0], args[1], args[2])]
        elif op in (OP_DP3, OP_DP4):
            n = 3 if op == OP_DP3 else 4
            d = f32(sum(f32(args[0][i] * args[1][i]) for i in range(n)))
            result = [d] * 4
        elif op == OP_RCP:
            result = [f32(1.0 / a) if a != 0 else math.inf for a in args[0]]
        elif op == OP_RSQ:
            result = [f32(1.0 / math.sqrt(a)) if a > 0 else math.nan for a in args[0]]
        elif op == OP_LG2:
            result = [f32(math.log2(a)) if a > 0 else math.nan for a in args[0]]
        elif op == OP_EX2:
            result = [f32(2.0 ** a) if not math.isnan(a) else math.nan for a in args[0]]
        elif op == OP_DIVSQR:
            result = [f32(a / math.sqrt(b)) if b > 0 else math.nan for a, b in zip(args[0], args[1])]
        else:
            raise AssertionError(f'unsupported opcode {op:#x} in the numeric witness')
        scale = SCALE.get(ins['scale'])
        if scale is None:
            raise AssertionError(f'unknown output scale {ins["scale"]}')
        result = [f32(r * scale) if not math.isnan(r) else r for r in result]
        old = regs.setdefault(ins['dst'], [math.nan] * 4)
        for lane in range(4):
            if ins['mask'] & (1 << lane):
                old[lane] = result[lane]
    if 0 not in regs:
        raise AssertionError('no output written')
    return regs[0]


# A NEGATIVE base in x and y, small positive in z, larger positive in w; every
# expectation below is exactly representable, so no rounding model is assumed.
T = [-1.5, -2.0, 0.25, 4.0]


def same(got, want, tag):
    ok = len(got) == len(want) and all(
        (math.isnan(a) and math.isnan(b)) or a == b for a, b in zip(got, want))
    require(ok, f'{tag}: decoded value {got} != {want}')


def check_values(work):
    blob = (work / 'fp_pow_int_exponents_f.fpo').read_bytes()
    same(execute_fp(blob, T), [f32(2.25), f32(-8.0), f32(4.0), f32(4.0)], 'fp_pow_int_exponents_f')
    # Control: the old chain on this input is NaN in x and y; the executor
    # must SEE that, or a NaN-blind executor could pass a NaN build.
    nan_chain = execute_fp((work / 'control-chain.fpo').read_bytes(), T)
    require(math.isnan(nan_chain[0]) and math.isnan(nan_chain[1]),
            f'control: the LG2/MUL/EX2 spelling on a negative base must decode as NaN, got {nan_chain}')
    blob = (work / 'fp_pow_zero_exponent_f.fpo').read_bytes()
    same(execute_fp(blob, T), [1.0, f32(-2.0), 0.0, 1.0], 'fp_pow_zero_exponent_f')
    blob = (work / 'fp_pow_vec_two_f.fpo').read_bytes()
    same(execute_fp(blob, T), [f32(2.25), f32(4.0), f32(0.0625), 1.0], 'fp_pow_vec_two_f')
    blob = (work / 'fp_pow_roots_f.fpo').read_bytes()
    same(execute_fp(blob, T), [f32(0.5), f32(0.5), 0.0, 1.0], 'fp_pow_roots_f')
    # Boy_HairFp's shape: pow(1 - pow(d, 2), 2) with a negative d must be finite.
    blob = (work / 'fp_pow_nested_strand_f.fpo').read_bytes()
    got = execute_fp(blob, T)
    require(all(math.isfinite(v) for v in got), f'fp_pow_nested_strand_f: non-finite {got}')
    twin = execute_fp((work / 'fp_pow_nested_strand_twin_f.fpo').read_bytes(), T)
    same(got, twin, 'fp_pow_nested_strand_f vs its s*s spelling')


def check_shapes(work):
    def lg2_ex2(tag, scale_code, negated):
        ins = decode_fp((work / f'{tag}.fpo').read_bytes())
        lg2 = [i for i in ins if i['op'] == OP_LG2]
        ex2 = [i for i in ins if i['op'] == OP_EX2]
        mul = [i for i in ins if i['op'] == OP_MUL]
        require(len(lg2) == 1 and len(ex2) == 1, f'{tag}: expected one LG2 and one EX2, got {len(lg2)}/{len(ex2)}')
        if lg2 and ex2:
            require(lg2[0]['scale'] == scale_code, f'{tag}: LG2 output scale {lg2[0]["scale"]}, expected {scale_code}')
            require(ex2[0]['srcs'][0]['negate'] == negated, f'{tag}: EX2 source negate {ex2[0]["srcs"][0]["negate"]}, expected {negated}')
            require(not mul, f'{tag}: the multiply must fold into the scale, found MUL')
    lg2_ex2('fp_pow_scaled_four_f', 2, False)
    lg2_ex2('fp_pow_scaled_eighth_f', 7, False)
    lg2_ex2('fp_pow_scaled_neg_two_f', 1, True)
    # exponent 5 and a variable keep the chain with an explicit MUL
    for tag in ('fp_pow_chain_five_f', 'fp_pow_variable_f'):
        ins = decode_fp((work / f'{tag}.fpo').read_bytes())
        ops = [i['op'] for i in ins]
        require(OP_LG2 in ops and OP_MUL in ops and OP_EX2 in ops, f'{tag}: expected LG2, MUL, EX2 chain, got {[hex(o) for o in ops]}')
        require(all(i['scale'] == 0 for i in ins if i['op'] == OP_LG2), f'{tag}: LG2 must not carry an output scale here')
    # the value rows must contain no LG2 at all: that is the whole fix
    for tag in ('fp_pow_int_exponents_f', 'fp_pow_vec_two_f', 'fp_pow_roots_f', 'fp_pow_zero_exponent_f'):
        ins = decode_fp((work / f'{tag}.fpo').read_bytes())
        require(all(i['op'] not in (OP_LG2, OP_EX2) for i in ins), f'{tag}: LG2/EX2 present; a constant 0/1/2/3/-1/+-0.5 exponent must not go through the log')


def execute_vp(blob, u):
    words, error = decode(blob)
    if error is not None:
        raise AssertionError(str(error))
    recs = {}
    prof, rev, total, n, hdr, prog, usz, uoff = struct.unpack_from('>8I', blob, 0)
    constants = {}
    for i in range(n):
        r = struct.unpack_from('>12I', blob, hdr + i * 48)
        name = blob[r[4]:blob.index(0, r[4])].decode() if r[4] else ''
        if name.startswith('internal-constant-'):
            constants[r[3]] = list(struct.unpack_from('>4f', blob, r[5]))
        elif name == 'u' and r[10]:
            constants[r[3]] = u
    regs = {'IN0': [1.0, -2.0, 3.0, 0.5]}

    def src(text):
        neg = text.startswith('-')
        text = text.lstrip('-')
        base, swizzle = text.rsplit('.', 1)
        value = constants[int(base[1:])] if base.startswith('C') and not base.startswith('C[') else regs[base]
        return [(-1 if neg else 1) * value['xyzw'.index(l)] for l in swizzle]
    for line in words:
        m = re.fullmatch(r'\d+ (\w+) dst=(\w+) mask=([xyzw-]+) src0=(\S+) src1=(\S+) src2=(\S+)', line)
        if not m:
            raise AssertionError('undecodable VP line ' + line)
        op, dst, mask, *sources = m.groups()
        if op == 'NOP':
            continue
        a = src(sources[0])
        if op == 'MOV':
            value = a
        elif op == 'RCP':
            value = [1.0 / v if v != 0 else math.inf for v in a]
        elif op == 'RSQ':
            value = [1.0 / math.sqrt(v) if v > 0 else math.nan for v in a]
        elif op == 'LG2':
            value = [math.log2(v) if v > 0 else math.nan for v in a]
        elif op == 'EX2':
            value = [2.0 ** v if not math.isnan(v) else math.nan for v in a]
        else:
            c = src(sources[2] if op == 'ADD' else sources[1])
            if op == 'MUL':
                value = [x * y for x, y in zip(a, c)]
            elif op == 'ADD':
                value = [x + y for x, y in zip(a, c)]
            elif op == 'MAD':
                value = [x * y + z for x, y, z in zip(a, c, src(sources[2]))]
            elif op in ('DP3', 'DP4'):
                value = [sum(x * y for x, y in zip(a[:int(op[-1])], c))] * 4
            else:
                raise AssertionError('unsupported VP instruction in numeric witness: ' + line)
        regs.setdefault(dst, [0.0] * 4)
        for lane in mask:
            if lane != '-':
                regs[dst]['xyzw'.index(lane)] = value['xyzw'.index(lane)]
    return regs.get('o1')


def check_vp(work):
    # vp_words decodes the VECTOR opcode of each instruction only, so the
    # vertex value row uses multiply-only exponents; the RCP / RSQ forms are
    # byte twins of 1/x and sqrt(x) in the shell guard (reference-identical).
    got = execute_vp((work / 'vp_pow_mul_v.vpo').read_bytes(), T)
    same([f32(v) if not math.isnan(v) else v for v in got], [2.25, -8.0, 0.0, 4.0], 'vp_pow_mul_v')


def main():
    work = Path(sys.argv[1])
    # Each section records its own failure instead of aborting the run, so a
    # parent binary that still emits the log chain reports the VALUE row it
    # fails (NaN on a negative base) rather than a decoder exception.
    for section in (check_values, check_shapes, check_vp):
        try:
            section(work)
        except Exception as error:  # noqa: BLE001 - any decoder/executor failure is this section's finding
            failures.append(f'{section.__name__}: {type(error).__name__}: {error}')
    if failures:
        for f in failures:
            print('FAIL:', f)
        sys.exit(1)
    print('PASS: pow_constant_check: 5 FP value rows on a negative base (+ NaN control), 5 shape rows, 1 VP value row on lanes y and w')


if __name__ == '__main__':
    main()
