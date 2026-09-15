"""Rectangular matrix guard checker (t_bc130064 / t_69aeaa84 / t_a5dbcca2).

Reference (sce-cgc 475) contract, measured 2026-09-15 in .local/probe-rect:
an RxC matrix is R rows of C-wide vectors; mul(M[RxC], v[C]) is one DP(C)
per row giving v[R]; mul(v[R], M[RxC]) accumulates the rows giving v[C];
M[i] is a row, M[i][j] a lane; constructors take one row vector of width C
per row (or R*C scalars); (float3x4)float4x4 keeps the first three rows;
transpose(3x4) is a 4x3.  Container records: parent type
kCgFloat1x1 + (rows-1)*4 + (cols-1) (3x4 = 1060, 4x3 = 1063, 2x4 = 1056),
one row record per ROW typed by the COLUMN count (1048 = float4, 1047 =
float3); half matrices use the float codes in FP.

Values are judged by executing OUR fragment ucode numerically on an exact
binary input (every product and sum is representable, so no rounding model
is assumed) and comparing with an independent row/column formula; vertex
programs are executed the same way with uniform rows supplied from their
own records.  Byte identity to the reference is a separate measurement and
is NOT assumed here (our FP output already differs from the reference in
input copies and export folds for square matrices).

Usage: rect_matrix_check.py <workdir-with-compiled-containers>
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


# --------------------------------------------------------------------------
# container records
def records(blob):
    prof, rev, total, n, hdr, prog, usz, uoff = struct.unpack_from('>8I', blob, 0)
    out = []
    for i in range(n):
        r = struct.unpack_from('>12I', blob, hdr + i * 48)

        def cstr(o):
            return blob[o:blob.index(0, o)].decode() if o else ''
        out.append({'name': cstr(r[4]), 'type': r[0], 'res': r[1], 'resIndex': r[3],
                    'default': r[5], 'isRef': r[10], 'raw': r})
    return out


def matrix_type(rows, cols):
    return 1049 + (rows - 1) * 4 + (cols - 1)


def check_fp_matrix_records(tag, blob, name, rows, cols):
    recs = {r['name']: r for r in records(blob)}
    require(name in recs, f'{tag}: no parent record for {name}')
    if name in recs:
        require(recs[name]['type'] == matrix_type(rows, cols),
                f'{tag}: {name} parent type {recs[name]["type"]}, expected {matrix_type(rows, cols)}')
    for r in range(rows):
        row = f'{name}[{r}]'
        require(row in recs, f'{tag}: missing row record {row}')
        if row in recs:
            require(recs[row]['type'] == 1044 + cols,
                    f'{tag}: {row} type {recs[row]["type"]}, expected {1044 + cols} (a {cols}-wide row)')
    require(f'{name}[{rows}]' not in recs, f'{tag}: {name} has more than {rows} rows')


# --------------------------------------------------------------------------
# fragment numeric executor: MOV MUL ADD MAD DP3 DP4 over TEX0 and inline constants
def execute_fp(blob, vector):
    regs = {}
    ended = False
    for w, const in instructions(ucode_words(blob)):
        ended = bool(w[0] & 1)
        op = (w[0] >> 24) & 63
        if op in (0, 0x3e):
            if ended:
                break
            continue
        if op not in (1, 2, 3, 4, 5, 6):
            raise AssertionError(f'unsupported numerical opcode {op}')
        if ((w[1] >> 18) & 7) != 7:
            raise AssertionError('unexpected predicated arithmetic')
        args = []
        for slot in range(1, ARITY[op] + 1):
            s = source(w, slot)
            if s['type'] == INPUT:
                if s['name'] != 'TEX0':
                    raise AssertionError('unexpected input ' + str(s['name']))
                data = vector
            elif s['type'] == CONST:
                if const is None:
                    raise AssertionError('missing inline block')
                data = [struct.unpack('>f', struct.pack('>I', x))[0] for x in const]
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
        if op in (5, 6):
            n = 3 if op == 5 else 4
            d = f32(sum(f32(args[0][i] * args[1][i]) for i in range(n)))
            result = [d] * 4
        else:
            result = []
            for lane in range(4):
                x = args[0][lane]
                if op in (2, 4):
                    x = f32(x * args[1][lane])
                if op == 3:
                    x = f32(x + args[1][lane])
                if op == 4:
                    x = f32(x + args[2][lane])
                result.append(x)
        dst, mask = (w[0] >> 1) & 63, (w[0] >> 9) & 15
        old = regs.setdefault(dst, [math.nan] * 4)
        for lane in range(4):
            if mask & (1 << lane):
                old[lane] = result[lane]
        if ended:
            break
    if not ended:
        raise AssertionError('missing END instruction')
    if 0 not in regs or not all(math.isfinite(x) for x in regs[0]):
        raise AssertionError('invalid output')
    return regs[0]


def swz(v, s):
    return [v['xyzw'.index(ch)] for ch in s]


def mat_vec(M, v):
    return [sum(row[k] * v[k] for k in range(len(v))) for row in M]


def vec_mat(v, M):
    return [sum(v[r] * M[r][c] for r in range(len(v))) for c in range(len(M[0]))]


def mat_mat(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(len(B))) for j in range(len(B[0]))] for i in range(len(A))]


def transpose(M):
    return [[M[r][c] for r in range(len(M))] for c in range(len(M[0]))]


T = [1.5, -2.0, 0.25, 4.0]
R0, R1, R2, R3 = T, swz(T, 'yzwx'), swz(T, 'zwxy'), swz(T, 'wxyz')
A0, A1, A2, A3 = swz(T, 'xyz'), swz(T, 'yzw'), swz(T, 'zwx'), swz(T, 'wxy')
M34 = [R0, R1, R2]
P43 = [A0, A1, A2, A3]
PB = [A0, A1, A0, A1]
FP_EXPECT = {
    'fp_rect_ctor_m34_mul_f': mat_vec(M34, T) + [1.0],
    'fp_rect_ctor_m43_mul_f': mat_vec(P43, A0),
    'fp_rect_ctor_v4_m43_f': vec_mat(T, P43) + [1.0],
    'fp_rect_ctor_v3_m34_f': vec_mat(A0, M34),
    'fp_rect_ctor_index_f': [R1[2], R2[0], R0[3], 1.0],
    'fp_rect_brace_m43_f': vec_mat(T, PB) + [1.0],
    'fp_rect_ctor_m43_twin_f': vec_mat(T, PB) + [1.0],
    'fp_rect_brace_row_f': A1 + [1.0],
    'fp_rect_transpose_f': mat_vec(transpose(M34), A0),
    'fp_rect_narrow_cast_f': mat_vec([R0, R1, R2, R3][:3], T) + [1.0],
    'fp_rect_matmul_f': mat_vec(mat_mat(M34, P43), A0) + [1.0],
    # CSE used to merge the vec3 shuffle t.yzw into the vec4 t.yzwx (same
    # operand and mask); the sum below reads both.
    'fp_rect_prefix_shuffle_cse_f': [A1[0] + R1[0], A1[1] + R1[1], A1[2] + R1[2], 1.0 + R1[3]],
}
FP_RECORDS = {
    'fp_rect_uniform_m34_f': [('M', 3, 4)],
    'fp_rect_uniform_v3_m34_f': [('M', 3, 4)],
    'fp_rect_uniform_v4_m43_f': [('P', 4, 3)],
    'fp_rect_uniform_m43_v3_f': [('P', 4, 3)],
    'fp_rect_uniform_f2x4_h3x4_f': [('A', 2, 4), ('B', 3, 4)],
    'fp_rect_row_access_f': [('M', 3, 4)],
}
# Distinct control: the same executor must reject a wrong expectation, so a
# value row cannot pass by an executor that returns the expectation itself.
CONTROL_WRONG = {'fp_rect_ctor_m34_mul_f': [0.0, 0.0, 0.0, 0.0]}


# --------------------------------------------------------------------------
# vertex numeric executor (uniform rows from the container's own records)
def bone_row(element, row):
    return [(element + 1) * 10 + row * 3 + col / 4 for col in range(4)]


def execute_vp(blob, uniforms, *, all_outputs=False):
    recs = records(blob)
    words, error = decode(blob)
    if error is not None:
        raise AssertionError(str(error))
    constants = {}
    for r in recs:
        if r['name'].startswith('internal-constant-'):
            constants[r['resIndex']] = list(struct.unpack_from('>4f', blob, r['default']))
        elif r['name'] in uniforms and r['isRef']:
            constants[r['resIndex']] = uniforms[r['name']]
    P = [1.0, -2.0, 3.0, 0.5]
    regs = {'IN0': P}

    def src(text):
        neg = text.startswith('-')
        text = text.lstrip('-')
        base, swizzle = text.rsplit('.', 1)
        if base.startswith('C['):
            raise AssertionError('relative addressing not expected here: ' + text)
        value = constants[int(base[1:])] if base.startswith('C') else regs[base]
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
    if all_outputs:
        return {name: value for name, value in regs.items() if name.startswith('o')}, P
    return regs.get('o1'), P


def check_vp_bones(tag, blob, elements, referenced):
    # Only REFERENCED elements take registers: three rows each, consecutive
    # from c[256] in element order (reference: bones[1] alone sits at c[256];
    # bones[0] and bones[1] both read sit at c[256] and c[259]).
    recs = {r['name']: r for r in records(blob)}
    base = {}
    next_reg = 256
    for e in range(elements):
        if e in referenced:
            base[e] = next_reg
            next_reg += 3
    for e in range(elements):
        parent = f'bones[{e}]'
        require(parent in recs and recs[parent]['type'] == 1060, f'{tag}: {parent} parent record/type')
        for r in range(3):
            row = f'{parent}[{r}]'
            require(row in recs, f'{tag}: missing {row}')
            if row not in recs:
                continue
            rec = recs[row]
            require(rec['type'] == 1048, f'{tag}: {row} type {rec["type"]}, expected 1048')
            if e in referenced:
                require(rec['isRef'] == 1 and rec['res'] == 2178 and rec['resIndex'] == base[e] + r,
                        f'{tag}: {row} should be referenced at c[{base[e] + r}], got res {rec["res"]} idx {rec["resIndex"]} isRef {rec["isRef"]}')
            else:
                require(rec['isRef'] == 0 and rec['res'] == 3256 and rec['resIndex'] == 0xffffffff,
                        f'{tag}: {row} should be an unreferenced element record')
        require(f'bones[{e}][3]' not in recs, f'{tag}: {parent} has a fourth row')


def main():
    work = Path(sys.argv[1])
    for tag, want in FP_EXPECT.items():
        blob = (work / f'{tag}.fpo').read_bytes()
        try:
            got = execute_fp(blob, T)
        except AssertionError as e:
            failures.append(f'{tag}: {e}')
            continue
        want = [f32(x) for x in want]
        require(got == want, f'{tag}: decoded value {got} != {want}')
    for tag, wrong in CONTROL_WRONG.items():
        got = execute_fp((work / f'{tag}.fpo').read_bytes(), T)
        require(got != wrong, f'{tag}: control failed - the executor returned the wrong expectation')
    for tag, mats in FP_RECORDS.items():
        blob = (work / f'{tag}.fpo').read_bytes()
        for name, rows, cols in mats:
            check_fp_matrix_records(tag, blob, name, rows, cols)
    # VP: per-element records and a numeric witness on the referenced element
    blob = (work / 'vp_rect_bones_array_v.vpo').read_bytes()
    check_vp_bones('vp_rect_bones_array_v', blob, 3, {1})
    uniforms = {f'bones[1][{r}]': bone_row(1, r) for r in range(3)}
    got, P = execute_vp(blob, uniforms)
    want = mat_vec([bone_row(1, r) for r in range(3)], P) + [1.0]
    require(got == want, f'vp_rect_bones_array_v: decoded value {got} != {want}')
    blob = (work / 'vp_rect_bones_blend_v.vpo').read_bytes()
    check_vp_bones('vp_rect_bones_blend_v', blob, 2, {0, 1})
    uniforms = {f'bones[{e}][{r}]': bone_row(e, r) for e in range(2) for r in range(3)}
    got, P = execute_vp(blob, uniforms)
    blended = [[bone_row(0, r)[c] * P[0] + bone_row(1, r)[c] * P[1] for c in range(4)] for r in range(3)]
    want = mat_vec(blended, P) + [1.0]
    require(got == want, f'vp_rect_bones_blend_v: decoded value {got} != {want}')
    if failures:
        for f in failures:
            print('FAIL:', f)
        sys.exit(1)
    print(f'PASS: rect_matrix_check: {len(FP_EXPECT)} FP value rows, {len(FP_RECORDS)} FP record rows, 2 VP array rows (records + values)')


if __name__ == '__main__':
    main()
