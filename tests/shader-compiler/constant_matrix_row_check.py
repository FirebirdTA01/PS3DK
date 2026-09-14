"""A constant matrix index selects a complete row, not one flattened scalar.

Reference475: every row equals its explicit vector twin, both profiles.
File-scope constants must match our explicit twins too. Local MatConstruct
rows take a different path: float rows are numerical controls; local half
element conversion is the separate pre-existing t_89de34c3 finding.
Negative/past-end constant rows produce C1068 'array index
out of bounds'; OUR refusal contract is exactly exit1 and no container.
The existing fp_const_matrix_fold_f witness at uv=(.5,.25) must compute
[-.5,-1.125,-2.75,1], where the pristine parent computes [1,0,0,1].
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from fp_vecmatmul_check import execute, require
from uniform_container_check import check_container


def compile_shader(compiler, root, name, text, profile, refusal=False):
    src, dst = root/(name+'.cg'), root/(name+'.bin')
    src.write_text(text)
    p = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                       capture_output=True, text=True, timeout=20)
    if refusal:
        require(p.returncode == 1 and not dst.exists(), f'{name}: expected refusal1/no container, got {p.returncode}')
        require('matrix row index out of bounds' in p.stderr, f'{name}: wrong diagnostic {p.stderr}')
        return None
    require(p.returncode == 0, f'{name}: compile exit {p.returncode}: {p.stderr}')
    return dst.read_bytes()


def verify_live(blob):
    want = [-.5,-1.125,-2.75,1]
    got, _ = execute(blob, [.5,.25,0,0])
    require(got == want, f'live matrix rows: {got} != {want}')


def main(compiler):
    compiler = str(Path(compiler).resolve())
    pairs = 0
    with tempfile.TemporaryDirectory(prefix='ps3dk-constant-matrix-row-') as temp:
        root = Path(temp)
        for base, width in (('float',2), ('float',3), ('float',4), ('half',3)):
            values = [str((i+1)/16) for i in range(width*width)]
            if base == 'half':
                # Every row survives; preserve half rounding, large finite
                # values and the separately decided subnormal-zero contract.
                values = ['1.00048828125','-1.00048828125','65520',
                          '0.00001','2.0009765625','3','4','5','6']
            decl = f'const static {base}{width}x{width} M={base}{width}x{width}('+','.join(values)+');'
            for row in range(width):
                for profile, semantic in (('sce_fp_rsx','COLOR'), ('sce_vp_rsx','POSITION')):
                    name = f'{base}{width}-{row}-{profile}'
                    def source(expr):
                        result = expr if width == 4 else f'float4({expr},'+('1)' if width == 3 else '0,1)')
                        return decl+f'float4 main():{semantic} {{return {result};}}'
                    blob = compile_shader(compiler, root, name, source(f'M[{row}]'), profile)
                    twin_expr = f'{base}{width}('+','.join(values[row*width:(row+1)*width])+')'
                    twin = compile_shader(compiler, root, name+'-twin', source(twin_expr), profile)
                    require(blob == twin, name+': explicit row twin differs')
                    require(not check_container(blob)['issues'], name+': container/ucode finding')
                    pairs += 1
        decl = 'const static float3x3 M=float3x3(1,2,3,4,5,6,7,8,9);'
        # Local float matrices are MatConstruct, not the IRConstant fallback
        # being repaired. Their instruction shapes differ from a literal twin
        # on the parent too; check their actual values, not byte identity.
        local_rows = 0
        for width in (2,3,4):
            values = [(i+1)/16 for i in range(width*width)]
            declaration = f'const float{width}x{width} M=float{width}x{width}('+','.join(map(str,values))+');'
            for row in range(width):
                expression = f'M[{row}]' if width == 4 else f'float4(M[{row}],'+('1)' if width == 3 else '0,1)')
                blob = compile_shader(compiler, root, f'local{width}-{row}',
                    'float4 main():COLOR {'+declaration+'return '+expression+';}', 'sce_fp_rsx')
                expected = values[row*width:(row+1)*width] + ([0,1] if width == 2 else [1] if width == 3 else [])
                actual, _ = execute(blob, [0,0,0,0])
                require(actual == expected, f'local{width}-{row}: {actual} != {expected}')
                local_rows += 1
        for profile, semantic in (('sce_fp_rsx','COLOR'), ('sce_vp_rsx','POSITION')):
            for name, expression, expected in (
                    ('swizzle','float4(M[1].yz,0,1)','float4(5,6,0,1)'),
                    ('expression','float4(M[2-1],1)','float4(4,5,6,1)'),
                    ('nested','float4(M[1][2],0,0,1)','float4(6,0,0,1)'),
                    ('float-index','float4(M[1.0],1)','float4(4,5,6,1)'),
                    ('fraction','float4(M[1.9],1)','float4(4,5,6,1)'),
                    ('negative-fraction','float4(M[-0.9],1)','float4(1,2,3,1)'),
                    ('last-fraction','float4(M[2.999],1)','float4(7,8,9,1)'),
                    ('float-expression','float4(M[1.7*1.2],1)','float4(7,8,9,1)')):
                def source(e):
                    return decl+f'float4 main():{semantic} {{return {e};}}'
                blob = compile_shader(compiler, root, name+profile, source(expression), profile)
                twin = compile_shader(compiler, root, name+profile+'-twin', source(expected), profile)
                require(blob == twin, name+profile+': composed row twin differs')
                pairs += 1
            for number, index in enumerate(('-1','3','-1.0','3.0','1e30','half(131072.0)','-half(131072.0)')):
                compile_shader(compiler, root, f'bounds{number}{profile}',
                    '// Reference475: error C1068: array index out of bounds.\n'+
                    decl+f'float4 main():{semantic} {{return float4(M[{index}],1);}}', profile, refusal=True)
        repo = Path(__file__).resolve().parents[2]
        live = (repo/'tools/rsx-cg-compiler/tests/shaders/fp_const_matrix_fold_f.cg').read_text()
        blob = compile_shader(compiler, root, 'live', live, 'sce_fp_rsx')
        verify_live(blob)
        # The same checker must reject a real wrong row, not only accept a
        # known good twin. Make the live expression reuse row0 for row1.
        wrong = compile_shader(compiler, root, 'wrong-row', live.replace('M[1]', 'M[0]'), 'sce_fp_rsx')
        try:
            verify_live(wrong)
        except AssertionError as error:
            require('live matrix rows:' in str(error), 'wrong-row control failed for another reason')
        else:
            raise AssertionError('wrong-row control passed the numerical checker')
    print(f'constant-matrix-row: PASS ({pairs} twins, {local_rows} local numerical controls, fourteen exact refusals, live numerical witness)')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: constant_matrix_row_check.py <compiler>')
    try:
        main(sys.argv[1])
    except (AssertionError, subprocess.TimeoutExpired) as error:
        sys.exit('FAIL: '+str(error))
