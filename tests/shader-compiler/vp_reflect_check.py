"""Authored VP reflect/expanded twins, checked against an independent formula.

--write-sources DIR writes the probes for a private reference compile.
--reference-dir DIR judges its <case>_<builtin|expanded>.vpo containers too.
No private shader source or reference binary is needed by the CI test.
"""
import argparse
import math
from pathlib import Path
import subprocess
import tempfile

from vp_pow_vector_check import evaluate, word_mutant

# name, width, I expression, N expression, I swizzle, N swizzle, I sign, N sign, N abs
CASES = [
    ('scalar_broadcast', 3, 'I.xxx', 'N.xxx', [0, 0, 0], [0, 0, 0], 1, 1, False),
    ('scalar_return', 3, 'I.xyz', 'N.xyz', [0, 1, 2], [0, 1, 2], 1, 1, False),
    ('vec2', 2, 'I.xy', 'N.xy', [0, 1], [0, 1], 1, 1, False),
    ('vec3', 3, 'I.xyz', 'N.xyz', [0, 1, 2], [0, 1, 2], 1, 1, False),
    ('vec4', 4, 'I', 'N', [0, 1, 2, 3], [0, 1, 2, 3], 1, 1, False),
    ('swizzle', 3, 'I.zyx', 'N.ywx', [2, 1, 0], [1, 3, 0], 1, 1, False),
    ('neg_i', 3, '-I.xyz', 'N.xyz', [0, 1, 2], [0, 1, 2], -1, 1, False),
    ('neg_n', 3, 'I.xyz', '-N.xyz', [0, 1, 2], [0, 1, 2], 1, -1, False),
    ('abs_n', 3, 'I.xyz', 'abs(N.xyz)', [0, 1, 2], [0, 1, 2], 1, 1, True),
]
VECTORS = [
    ([1.5, -.25, .5, -1.], [.25, -.5, .75, 1.]),
    ([-.5, 1., 2., .25], [-1., .25, -.5, .75]),
    ([.25, .5, .75, 1.], [0., 0., 0., 0.]),
    ([1., 0., 0., 0.], [0., 1., 0., 0.]),
    ([0., -1., 0., .5], [0., 1., 0., 0.]),
    ([1., 2., 3., 4.], [.5, -.25, .75, -1.]),
]


def source(case, expanded):
    name, width, incident, normal, *_ = case
    ty = 'float' if width == 1 else 'float' + str(width)
    expr = 'i - (2.0 * dot(n, i)) * n' if expanded else 'reflect(i, n)'
    if expanded and width == 2:
        expr = 'i - (2.0 * (n.x * i.x + n.y * i.y)) * n'
    output = {1: 'float4(r, 0., 0., 1.)', 2: 'float4(r, 0., 1.)',
              3: 'float4(r, 1.)', 4: 'r'}[width]
    # Explicit conversions keep this VP-lowering test separate from the
    # known scalar-overload and implicit vector-truncation frontend gaps.
    if name in ('scalar_return', 'scalar_broadcast'):
        output = 'float4(r.x, 0., 0., 1.)'
    return (f'void main(float4 p:POSITION, uniform float4 I, uniform float4 N, '
            f'out float4 op:POSITION, out float4 color:COLOR0) {{\n'
            f'  op=p; {ty} i={incident}; {ty} n={normal}; {ty} r={expr};\n'
            f'  color={output};\n}}\n')


def expected(case, incident, normal):
    name, width, _, _, isw, nsw, isig, nsig, absolute = case
    i = [isig * incident[k] for k in isw]
    n = [nsig * (abs(normal[k]) if absolute else normal[k]) for k in nsw]
    twice_dot = 2. * sum(x*y for x, y in zip(i, n))
    result = [x - twice_dot*y for x, y in zip(i, n)]
    if name in ('scalar_return', 'scalar_broadcast'):
        return [result[0], 0., 0., 1.]
    return result if width == 4 else result + [0.] * (3-width) + [1.]


def judge(blob, case):
    for incident, normal in VECTORS:
        outputs = evaluate(blob, {'I': incident, 'N': normal})
        assert outputs.get(0) == [0., 0., 0., 1.], 'POSITION changed'
        got = outputs.get(1)
        want = expected(case, incident, normal)
        assert got and len(got) == 4, 'missing COLOR0'
        assert all(x is not None and math.isfinite(x) and
                   math.isclose(x, y, rel_tol=2e-6, abs_tol=2e-6)
                   for x, y in zip(got, want)), (case[0], incident, normal, got, want)


def negate_dot(blob):
    """A sign error in the reduction must be observed by the value judge."""
    def pick(n, count, words):
        return (words[1] >> 22) & 31 in (5, 7)
    def edit(n, count, words):
        # source zero's NEG bit is field bit 16, word1 bit 7.
        words[1] ^= 1 << 7
    return word_mutant(blob, pick, edit)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler', nargs='?')
    parser.add_argument('--write-sources', type=Path)
    parser.add_argument('--reference-dir', type=Path)
    args = parser.parse_args()
    if args.write_sources:
        args.write_sources.mkdir(parents=True, exist_ok=True)
        for case in CASES:
            for expanded in (False, True):
                name = case[0] + ('_expanded' if expanded else '_builtin')
                (args.write_sources / (name + '.cg')).write_text(source(case, expanded))
        return
    assert args.compiler or args.reference_dir, 'compiler or reference directory required'
    passed, failures = 0, []
    with tempfile.TemporaryDirectory(prefix='vp-reflect-') as tmp:
        root = Path(tmp)
        for case in CASES:
            for expanded in (False, True):
                name = case[0] + ('_expanded' if expanded else '_builtin')
                blobs = []
                # The pinned oracle has only the float3 reflect overload.
                # Hand-expanded float2/float4 arithmetic is still valid.
                refused = case[0] in ('vec2', 'vec4') and not expanded
                if args.compiler:
                    src, dst = root / (name + '.cg'), root / (name + '.vpo')
                    src.write_text(source(case, expanded))
                    run = subprocess.run([args.compiler, '-p', 'sce_vp_rsx', '--emit-container', str(dst), str(src)],
                                         capture_output=True, text=True, timeout=30)
                    if refused:
                        if run.returncode == 1 and not dst.exists():
                            passed += 1
                        else:
                            failures.append(f'{name}: expected refusal without artifact, rc={run.returncode}')
                    elif run.returncode != 0 or not dst.exists():
                        failures.append(f'{name}: compile rc={run.returncode}: {run.stderr.strip()}')
                    else:
                        blobs.append(('ours', dst.read_bytes()))
                if args.reference_dir:
                    dst = args.reference_dir / (name + '.vpo')
                    if refused:
                        diagnostic = (args.reference_dir / (name + '.stderr')).read_text(encoding='utf-8-sig')
                        code = 'C1115' if case[0] == 'vec2' else 'C1101'
                        if not dst.exists() and code in diagnostic and 'reflect' in diagnostic:
                            passed += 1
                        else:
                            failures.append(f'reference {name}: missing overload-refusal evidence')
                    elif dst.exists():
                        blobs.append(('reference', dst.read_bytes()))
                    else:
                        failures.append(f'reference {name}: no container')
                for origin, blob in blobs:
                    try:
                        judge(blob, case)
                        passed += 1
                        # Both DP3 and DP4 reduction signs must matter. Other
                        # widths have different measured reduction shapes.
                        if case[0] in ('vec3', 'vec4'):
                            mutant = negate_dot(blob)
                            assert mutant != blob, 'dot-sign mutation changed nothing'
                            try:
                                judge(mutant, case)
                            except AssertionError:
                                passed += 1
                            else:
                                raise AssertionError('dot-sign mutant accepted')
                    except AssertionError as error:
                        failures.append(f'{origin} {name}: {error}')
    for failure in failures:
        print('FAIL:', failure)
    print(f'vp reflect: tests={passed+len(failures)} pass={passed} fail={len(failures)}; {len(VECTORS)} value vectors per container')
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
