"""Value checks for VP refract; zero RSQ/RCP requires separate physical evidence.

The encoded comparison control distinguishes SGT from SGE at equality without
claiming to model the scalar unit at zero. Sources and reference containers can
be exported/imported for private oracle checks; no oracle source is required.
"""
import argparse
import math
import re
from pathlib import Path
import struct
import subprocess
import tempfile

from vp_pow_vector_check import evaluate

CASES = {f'uniform{w}': (w, 'I', 'N', 'eta') for w in range(1, 5)}
CASES.update({
    'swizzled2': (2, 'I.yw', 'N.wx', 'eta'),
    'repeated2': (2, 'I.yy', 'N.ww', 'eta'),
    'one_repeated2': (2, 'I.yy', 'N.xy', 'eta'),
    'swizzled3': (3, 'I.zyx', 'N.yzx', 'eta'),
    'neg_i': (3, '-I', 'N', 'eta'),
    'neg_n': (3, 'I', '-N', 'eta'),
    'abs_n': (3, 'I', 'abs(N)', 'eta'),
    'neg_eta': (3, 'I', 'N', '-eta'),
    'abs_eta': (3, 'I', 'N', 'abs(eta)'),
    'selected_eta': (3, 'I', 'N', 'E.y'),
    'attribute3': (3, 'a', 'b', 'eta'),
    'inplace_i': (3, 'I', 'N', 'eta'),
    'inplace_n': (3, 'I', 'N', 'eta'),
})
# Finite nonzero-root values, including TIR and normal incidence. Exact-zero
# inputs are exported separately and never silently counted as model passes.
INPUTS = [
    ([1., 0., 0., .25], [0., 1., 0., -.5], .5),
    ([1., 0., 0., .25], [0., 1., 0., -.5], 2.),
    ([0., -1., 0., .25], [0., 1., 0., -.5], .5),
    ([.5, -.75, .25, -.125], [0., 1., 0., .5], 0.),
    ([.25, -.5, .75, -.125], [-.5, .25, -.25, .5], -.5),
    ([1., 0., 0., .25], [0., 1., 0., -.5], 1. - 2.**-24),
    ([1., 0., 0., .25], [0., 1., 0., -.5], 1. + 2.**-23),
]


def select(expr, width):
    if '.' in expr:
        return ["xyzw".index(x) for x in expr.split('.')[1]]
    return list(range(width))


def source(name):
    width, ie, ne, eta = CASES[name]
    def typed(expr):
        if '.' in expr:
            return expr
        suffix = '.' + 'xyzw'[:width] if width < 4 else ''
        return re.sub(r'\b[INab]\b', lambda m: m[0] + suffix, expr)
    call = f'refract({typed(ie)},{typed(ne)},{eta})'
    typename = 'float' if width == 1 else f'float{width}'
    output = {1: 'float4(q,0.,0.,1.)', 2: 'float4(q,0.,1.)',
              3: 'float4(q,1.)', 4: 'q'}[width]
    body = f'{typename} q={call};color={output};'
    if name.startswith('inplace_'):
        target = 'I' if name == 'inplace_i' else 'N'
        body = f'float3 r={target}.xyz;r=refract(' + ('r,N.xyz' if target == 'I' else 'I.xyz,r') + ',eta);color=float4(r,1.);'
    return ('void main(float4 p:POSITION,float4 a:NORMAL,float4 b:TEXCOORD0,'
            'uniform float4 I,uniform float4 N,uniform float eta,uniform float4 E,'
            'out float4 op:POSITION,out float4 color:COLOR0){op=p;' + body + '}\n')


def comparison_blob(op=18):
    """Hand-encoded COLOR0 = input0.xyzw > input0.wwww (no compiler)."""
    a, b = 2 | 0x1b00, 2 | 0xff00
    words = (1 << 30, (op << 22) | (a >> 9),
             ((a & 511) << 23) | (b << 6), (15 << 13) | (1 << 2) | 1)
    return struct.pack('>8I', 7003, 0, 0, 0, 32, 0, 16, 32) + struct.pack('>4I', *words)


def comparison_controls():
    def judge(blob):
        got = evaluate(blob, {}, {0: [-1., 0., 1., 0.]}, binary32=True)
        assert got[1] == [0., 0., 1., 0.], got
    judge(comparison_blob())
    # Both operations are modeled: this mutant must fail by VALUE, not because
    # the evaluator refuses an unknown opcode.
    got = evaluate(comparison_blob(12), {}, {0: [-1., 0., 1., 0.]}, binary32=True)
    assert got[1] == [0., 1., 1., 1.], got
    try:
        judge(comparison_blob(12))
    except AssertionError:
        return 3
    raise AssertionError('SGT -> SGE equality mutant accepted')


def expected(name, incident, normal, eta):
    width, ie, ne, ee = CASES[name]
    def operand(expr, vector):
        values = [vector[j] for j in select(expr, width)]
        if expr.startswith('-'):
            values = [-x for x in values]
        if expr.startswith('abs'):
            values = [abs(x) for x in values]
        return values
    i, n = operand(ie, incident), operand(ne, normal)
    if ee == '-eta': eta = -eta
    if ee == 'abs(eta)': eta = abs(eta)
    d = sum(a*b for a, b in zip(i, n))
    k = 1. - eta*eta*(1. - d*d)
    assert k != 0., 'zero-root case requires physical evidence'
    result = [eta*a - (eta*d + math.sqrt(abs(k)))*b for a, b in zip(i, n)] if k > 0. else [0.]*width
    return result + ([0.]*(3-width) + [1.] if width < 4 else [])


def judge(blob, name):
    for incident, normal, eta in INPUTS:
        want = expected(name, incident, normal, eta)
        got = evaluate(blob, {'I': incident, 'N': normal, 'eta': [eta]*4,
                             'E': [7., eta, 3., 5.]},
                       {0: [.25, .5, .75, 1.], 2: incident, 8: normal}, binary32=True)
        assert got.get(0) == [.25, .5, .75, 1.], 'POSITION changed'
        color = got.get(1)
        assert color and all(x is not None and math.isfinite(x) for x in color), 'missing/nonfinite COLOR0'
        assert all(abs(x-y) < 2e-5 for x,y in zip(color, want)), (name, incident, normal, eta, color, want)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler', nargs='?')
    parser.add_argument('--write-sources', type=Path)
    parser.add_argument('--reference-dir', type=Path)
    parser.add_argument('--controls-only', action='store_true')
    args = parser.parse_args()
    if args.write_sources:
        args.write_sources.mkdir(parents=True, exist_ok=True)
        for name in CASES: (args.write_sources/(name+'.cg')).write_text(source(name))
        return
    controls = comparison_controls()
    if args.controls_only:
        print(f'VP comparison controls: tests={controls} pass={controls} fail=0')
        return
    assert args.compiler or args.reference_dir
    failures = []; passed = 0; debts = 0
    with tempfile.TemporaryDirectory(prefix='vp-refract-') as tmp:
        for name in CASES:
            try:
                if args.compiler:
                    src, dst = Path(tmp)/(name+'.cg'), Path(tmp)/(name+'.vpo')
                    src.write_text(source(name))
                    run = subprocess.run([args.compiler, '-p', 'sce_vp_rsx', '--emit-container', str(dst), str(src)], capture_output=True, text=True, timeout=30)
                    if name == 'uniform1':
                        assert run.returncode == 1 and not dst.exists() and "ambiguous overloaded function reference 'refract'" in run.stderr, f'expected scalar overload refusal exit 1, got {run.returncode}: {run.stderr}'
                        print('KNOWN-DEBT: uniform1 scalar overload ambiguity (t_dfd4a7fb)')
                        debts += 1
                        continue
                    assert run.returncode == 0 and dst.exists(), run.stderr
                else:
                    dst = args.reference_dir/(name+'.vpo')
                judge(dst.read_bytes(), name)
                passed += 1
            except (AssertionError, OSError, ZeroDivisionError) as error:
                failures.append(name + ': ' + str(error))
    for failure in failures: print('FAIL:', failure)
    print(f'VP refract: tests={len(CASES)} pass={passed} fail={len(failures)} known-debt={debts}; encoded controls={controls}; zero-root UNJUDGED (physical gate)')
    raise SystemExit(bool(failures))


if __name__ == '__main__': main()
