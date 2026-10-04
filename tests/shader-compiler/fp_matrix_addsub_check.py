"""FP same-shape float matrix Add/Sub: decoded values and vector twins.

All inputs are small binary fractions, so the independent row formula is
exact despite instruction scheduling and permitted MAD contraction. Uniform
rows are uploaded through their own container relocation records.
Use --prepare-only WORK to create reviewable probes without running a compiler.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from fp_eval import evaluate, Unmodelled
from fp_sources import unswap
from uniform_container_check import Container, check_container


def require(ok, message):
    if not ok:
        raise AssertionError(message)


INPUTS = [
    {'TEX0': [.125, -.25, .5, -.75], 'TEX1': [.375, .625, -.125, .875]},
    {'TEX0': [-1, .75, -.5, .25], 'TEX1': [.5, -.375, .125, -.625]},
    {'TEX0': [2, -3, 4, -5], 'TEX1': [-.25, .5, -.75, 1]},
]


def swizzle(vector, text):
    sign = -1 if text.startswith('-') else 1
    return [sign * vector['xyzw'.index(c)] for c in text.lstrip('-')]


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    cases = []
    # Negative source modifiers are exercised on selected RHS rows without
    # introducing the separately unsupported unary negation of a matrix.
    for rows, cols, op, form in (
        (4, 3, '-', 'constructed'), (4, 3, '+', 'constructed'),
        (2, 4, '-', 'constructed'), (2, 4, '+', 'constructed'),
        (4, 3, '-', 'uniform'), (2, 4, '+', 'uniform'),
    ):
        name = f'{form}-{rows}x{cols}-' + ('sub' if op == '-' else 'add')
        shape = f'float{rows}x{cols}'
        left_swz = ['xyz', 'zxy', 'yxz', 'zyx'] if cols == 3 else ['xyzw', 'wzyx']
        right_swz = ['-zyx', 'xzy', '-yzx', 'zxy'] if cols == 3 else ['-yzwx', 'zwxy']
        uniforms = {}
        if form == 'constructed':
            def expr(base, text):
                return ('-' if text.startswith('-') else '') + base + '.' + text.lstrip('-')
            left_expr = [expr('p', s) for s in left_swz]
            right_expr = [expr('q', s) for s in right_swz]
            prefix = ''
            parameters = 'float4 p:TEXCOORD0, float4 q:TEXCOORD1'
            declarations = (f'{shape} A={shape}(' + ','.join(left_expr) + ');'
                            f'{shape} B={shape}(' + ','.join(right_expr) + ');')
        else:
            # Global A and entry parameter B cover both binding paths.
            prefix = f'uniform {shape} A;'
            parameters = f'float4 p:TEXCOORD0, uniform {shape} B'
            declarations = ''
            left_expr = [f'A[{r}]' for r in range(rows)]
            right_expr = [f'B[{r}]' for r in range(rows)]
            for r in range(rows):
                uniforms[f'A[{r}]'] = [(2*r+c+1)/8 for c in range(cols)]
                uniforms[f'B[{r}]'] = [(-1 if (r+c)%2 else 1)*(r+3*c+2)/16 for c in range(cols)]
        result = '+'.join(f'{1 << r}.0*C[{r}]' for r in range(rows))
        twin_result = '+'.join(f'{1 << r}.0*(({left_expr[r]}){op}({right_expr[r]}))' for r in range(rows))
        wrap = lambda expression: f'float4({expression},1.0)' if cols == 3 else expression
        source = prefix + f'float4 main({parameters}):COLOR{{' + declarations + f'{shape} C=A{op}B;return {wrap(result)};}}\n'
        twin = prefix + f'float4 main({parameters}):COLOR{{return {wrap(twin_result)};}}\n'
        expected = []
        for inputs in INPUTS:
            left = [uniforms[f'A[{r}]'] if form == 'uniform' else swizzle(inputs['TEX0'], left_swz[r]) for r in range(rows)]
            right = [uniforms[f'B[{r}]'] if form == 'uniform' else swizzle(inputs['TEX1'], right_swz[r]) for r in range(rows)]
            sign = -1 if op == '-' else 1
            values = [sum((1 << r)*(left[r][c]+sign*right[r][c]) for r in range(rows)) for c in range(cols)]
            expected.append(values + ([1.0] if cols == 3 else []))
        case = dict(name=name, profile='sce_fp_rsx', uniforms=uniforms, inputs=INPUTS, expected=expected)
        for suffix, text in (('', source), ('-twin', twin)):
            (root/(name+suffix+'.cg')).write_text(text)
        cases.append(case)

    # Former scalar-arithmetic refusals now have independent row/value controls.
    for name, expression, row, expected in (
        ('scalar-left-multiply', '.5*A', '.5*A[0]', [.125,-.25,.5,1.]),
        ('scalar-right-multiply', 'A*.5', 'A[0]*.5', [.125,-.25,.5,1.]),
        ('scalar-add', 'A+.5', 'A[0]+.5', [.75,0.,1.5,1.]),
    ):
        for suffix, result in (('', f'({expression})[0]'), ('-twin', row)):
            (root/(name+suffix+'.cg')).write_text(
                f'uniform float3x3 A;float4 main():COLOR{{return float4({result},1);}}\n')
        cases.append(dict(name=name, profile='sce_fp_rsx', uniforms={'A[0]':[.25,-.5,1.]},
                          inputs=[{}], expected=[expected]))
    # Incompatible shapes and unmodelled half-matrix precision still refuse.
    # VP subtraction has its own value witness in vp-matrix-arithmetic-test.
    refusal_sources = {
        'different-shapes': ('sce_fp_rsx', 'uniform float4x3 A;uniform float3x4 B;float4 main():COLOR{return float4((A-B)[0],1);}', None),
        'half-add': ('sce_fp_rsx', 'uniform half3x3 A;uniform half3x3 B;float4 main():COLOR{return float4((A+B)[0],1);}', 'matrix arithmetic'),
        'mixed-half-add': ('sce_fp_rsx', 'uniform half3x3 A;uniform float3x3 B;float4 main():COLOR{return float4((A+B)[0],1);}', 'matrix arithmetic'),
    }
    for name, (profile, source, diagnostic) in refusal_sources.items():
        (root/(name+'.cg')).write_text(source+'\n')
        cases.append(dict(name=name, profile=profile, refusal=True, diagnostic=diagnostic))
    (root/'manifest.json').write_text(json.dumps(cases, indent=2)+'\n')
    return cases


def upload(blob, values):
    container = Container(blob)
    by_name = {record['name']: record for record in container.records}
    patched = bytearray(blob)
    for name, lanes in values.items():
        require(name in by_name and by_name[name]['offsets'], name+': missing live uniform row')
        for offset in by_name[name]['offsets']:
            for lane, value in enumerate(lanes):
                word = struct.unpack('>I', struct.pack('>f', value))[0]
                struct.pack_into('>I', patched, container.ucode+offset+4*lane, unswap(word))
    return bytes(patched)


def run(compiler, root, cases):
    failures, reports = [], []
    for case in cases:
        name = case['name']
        for suffix in ('',) if case.get('refusal') else ('', '-twin'):
            tag = name+suffix
            output = root/(tag+'.bin')
            # A previous run must not masquerade as newly emitted output.
            if output.exists():
                output.unlink()
            try:
                result = subprocess.run([compiler, '-p', case['profile'], '--emit-container',
                                         str(output), str(root/(tag+'.cg'))],
                                        capture_output=True, text=True, timeout=20)
                (root/(tag+'.log')).write_text(result.stdout+result.stderr)
                report = dict(name=tag, status=result.returncode)
                if case.get('refusal'):
                    require(result.returncode == 1 and not output.exists(), tag+': expected exit1 and no output')
                    if case['diagnostic']:
                        require(case['diagnostic'] in result.stderr, tag+': wrong refusal diagnostic: '+result.stderr)
                    report['refusal_verified'] = True
                else:
                    require(result.returncode == 0 and output.exists(), tag+': compilation failed: '+result.stderr)
                    blob = output.read_bytes()
                    require(not check_container(blob)['issues'], tag+': container consistency failure')
                    patched = upload(blob, case['uniforms'])
                    got = [evaluate(patched, inputs) for inputs in case['inputs']]
                    require(got == case['expected'], tag+': decoded values '+str(got)+' expected '+str(case['expected']))
                    report['values'] = got
                reports.append(report)
            except (AssertionError, Unmodelled, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                failures.append(tag+': '+str(error))
    (root/'RESULT.json').write_text(json.dumps(dict(reports=reports, failures=failures), indent=2)+'\n')
    require(not failures, '\n'.join(failures))
    print('PASS: fp-matrix-addsub (9 matrix/vector pairs, 42 numeric checks, 3 refusal controls)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', nargs='?')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--prepare-only', type=Path)
    args = parser.parse_args()
    if args.prepare_only:
        prepare(args.prepare_only.resolve())
        print(args.prepare_only.resolve())
        return
    require(args.compiler is not None, 'compiler path required')
    compiler = str(Path(args.compiler).resolve())
    if args.work_dir:
        root = args.work_dir.resolve()
        run(compiler, root, prepare(root))
    else:
        with tempfile.TemporaryDirectory(prefix='ps3dk-fp-matrix-addsub-') as temporary:
            root = Path(temporary)
            run(compiler, root, prepare(root))


if __name__ == '__main__':
    try:
        main()
    except AssertionError as error:
        raise SystemExit('FAIL: '+str(error))
