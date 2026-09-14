"""Numerical FP vecmatmul guard; square types only (rectangular: t_a5dbcca2).

Reference475 uniform/default/constant probes all use row order 1,0,2,3.
The explicit twin uses vector broadcasts and MADs in that measured order.
Reference byte identity is a separate measurement, not assumed by this guard.
"""
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from fp_sources import ARITY, CONST, INPUT, TEMP, instructions, source, ucode_words
from uniform_container_check import Container, check_container
from fp_sources import unswap


def require(ok, why):
    if not ok:
        raise AssertionError(why)


def f32(v):
    return struct.unpack('>f', struct.pack('>f', v))[0]


def execute(blob, vector):
    regs = {}
    ops = []
    ended = False
    for w, const in instructions(ucode_words(blob)):
        ended = bool(w[0] & 1)
        op = (w[0] >> 24) & 63
        require(op in (0, 1, 2, 3, 4, 0x3e), f'unsupported numerical opcode {op}')
        if op in (0, 0x3e):
            if ended:
                break
            continue
        require(((w[1] >> 18) & 7) == 7, 'unexpected predicated arithmetic')
        require(not (w[0] & ((3 << 22) | (1 << 7) | (3 << 30))),
                'unsupported precision or output modifier')
        require(not (w[2] & (15 << 28)), 'unsupported scale or branch')
        args = []
        for slot in range(1, ARITY[op]+1):
            s = source(w, slot)
            require(not s['half'], 'half register in float fixture')
            if s['type'] == INPUT:
                require(s['name'] == 'TEX0', 'unexpected input')
                data = vector
            elif s['type'] == CONST:
                require(const is not None, 'missing inline block')
                data = [struct.unpack('>f', struct.pack('>I', x))[0] for x in const]
            else:
                require(s['type'] == TEMP and s['reg'] in regs, 'uninitialized temporary')
                data = regs[s['reg']]
            lanes = [data[(s['swizzle'] >> (2*i)) & 3] for i in range(4)]
            if s['abs']:
                lanes = [abs(v) for v in lanes]
            if s['negate']:
                lanes = [-v for v in lanes]
            args.append(lanes)
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
        old = regs.setdefault(dst, [math.nan]*4)
        for lane in range(4):
            if mask & (1 << lane):
                old[lane] = result[lane]
        if op in (2,4):
            ops.append(op)
        if ended:
            break
    require(ended, 'missing END instruction')
    require(0 in regs and all(math.isfinite(x) for x in regs[0]), 'invalid output')
    return regs[0], ops


def verify_values(blob, t, matrix, swizzle, tag):
    width = len(matrix)
    v = [t['xyzw'.index(ch)] for ch in swizzle]
    # Binary-exact fixtures keep all products/sums exactly representable, so
    # this independent column formula does not assume MAD rounding semantics.
    want = [sum(v[r]*matrix[r][c] for r in range(width)) for c in range(width)]
    want += [0.0,1.0] if width == 2 else ([1.0] if width == 3 else [])
    got, ops = execute(blob, t)
    require(got == want, f'{tag}: decoded value {got} != {want}')
    require(ops == [2]+[4]*(width-1), tag+': wrong MUL/MAD shape')


def patch_matrix(blob, matrix):
    """Simulate runtime uniform upload using the container's relocation lists."""
    container = Container(blob)
    patched = bytearray(blob)
    for row, values in enumerate(matrix):
        record = next(r for r in container.records if r['name'] == f'M[{row}]')
        require(record['offsets'], 'runtime matrix row has no patch location')
        for offset in record['offsets']:
            for lane, value in enumerate(values):
                word = struct.unpack('>I', struct.pack('>f', value))[0]
                struct.pack_into('>I', patched, container.ucode+offset+4*lane, unswap(word))
    return bytes(patched)


def mutation_controls(blob, matrix, swizzle, t):
    container = Container(blob)
    locations = [container.ucode+r['offsets'][0] for r in container.records
                 if r['name'] in ('M[0]', 'M[1]')]
    require(len(locations) == 2, 'mutation needs two distinguishable matrix rows')
    variants = {}
    a, b = locations
    mutant = bytearray(blob)
    mutant[a:a+16], mutant[b:b+16] = blob[b:b+16], blob[a:a+16]
    variants['row swap'] = mutant
    mutant = bytearray(blob)
    struct.pack_into('>I', mutant, a, unswap(0x7fc00000))
    variants['NaN'] = mutant
    mutant = bytearray(blob)
    first = unswap(struct.unpack_from('>I', mutant, container.ucode)[0])
    struct.pack_into('>I', mutant, container.ucode, unswap(first | 1))
    variants['early END'] = mutant
    # Mutate ONLY the first MUL's vector selector, leaving every inline word
    # and relocation intact: a container/default-only checker cannot see it.
    mutant = bytearray(blob)
    at = container.ucode
    for words, const in instructions(ucode_words(blob)):
        if ((words[0] >> 24) & 63) == 2:
            struct.pack_into('>I', mutant, at+4, unswap(words[1] ^ (0x55 << 9)))
            break
        at += 32 if const is not None else 16
    else:
        raise AssertionError('selector mutant found no MUL')
    variants['selector'] = mutant
    for name, mutant in variants.items():
        try:
            verify_values(bytes(mutant), t, matrix, swizzle, name)
        except AssertionError as error:
            require('decoded value' in str(error) or 'invalid output' in str(error),
                    name+': failed for the wrong reason: '+str(error))
        else:
            raise AssertionError(name+': numerical checker accepted corruption')


def compile_blob(compiler, root, name, text, refuse=False):
    src, dst = root/(name+'.cg'), root/(name+'.fpo')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=20)
    if refuse:
        require(run.returncode == 1 and not dst.exists(), f'{name}: refusal status {run.returncode}')
        require('unknown type' in run.stderr, f'{name}: wrong refusal {run.stderr}')
        return None
    require(run.returncode == 0, f'{name}: compile exit {run.returncode}: {run.stderr}')
    return dst.read_bytes()


def main(compiler):
    compiler = str(Path(compiler).resolve())
    count = 0
    with tempfile.TemporaryDirectory(prefix='ps3dk-fp-vecmatmul-') as tmp:
        root = Path(tmp)
        for width in (2,3,4):
            matrix = [[(-1 if (r+c)%3 == 2 else 1)*(r*width+c+1)/32 for c in range(width)] for r in range(width)]
            values = ','.join(str(x) for row in matrix for x in row)
            typ = f'float{width}x{width}'
            for storage in ('default', 'const', 'runtime'):
                decl = (f'uniform {typ} M;\n' if storage == 'runtime' else
                        f'{"uniform" if storage == "default" else "static const"} {typ} M={typ}({values});\n')
                for swizzle in ('xyzw'[:width], 'wzyx'[:width]):
                    vector = 't.'+swizzle
                    broadcasts = [f'v.{"xyzw"[r]*width}' for r in range(width)]
                    # Static matrix indexing has a separate pre-existing wrong
                    # scalar fold (t_a3f93c78). Spell those rows literally.
                    rows = [f'M[{r}]' if storage != 'const' else
                            f'float{width}('+','.join(str(x) for x in matrix[r])+')'
                            for r in range(width)]
                    expr = f'{broadcasts[1]}*{rows[1]} + {broadcasts[0]}*{rows[0]}'
                    for row in range(2,width):
                        expr = f'({expr}) + {broadcasts[row]}*{rows[row]}'
                    def body(e):
                        result = e if width == 4 else f'float4({e},'+('1.0)' if width == 3 else '0.0,1.0)')
                        return decl+f'float4 main(float4 t:TEXCOORD0):COLOR {{ float{width} v={vector}; return {result}; }}\n'
                    tag = f'{width}-{storage}-{swizzle}'
                    blob = compile_blob(compiler, root, tag, body('mul(v,M)'))
                    twin = compile_blob(compiler, root, tag+'-twin', body(expr))
                    require(blob == twin, tag+': explicit row twin differs')
                    require(not check_container(blob)['issues'], tag+': container/ucode disagreement')
                    evaluated = patch_matrix(blob, matrix) if storage == 'runtime' else blob
                    for t in ((1/8,3/16,-5/32,7/16), (-3/4,1/2,5/8,-1/4)):
                        verify_values(evaluated, t, matrix, swizzle, tag)
                    if storage == 'default' and width == 3 and swizzle == 'xyz':
                        mutation_controls(blob, matrix, swizzle, t)
                    count += 1
        compile_blob(compiler, root, 'rectangular', 'uniform float3x4 M; float4 main(float3 t:TEXCOORD0):COLOR{return mul(t,M);}', refuse=True)
    print(f'fp-vecmatmul: PASS ({count} twins and decoded numerical cases; rectangular refusal)')


if __name__ == '__main__':
    try:
        main(sys.argv[1])
    except (AssertionError, subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
