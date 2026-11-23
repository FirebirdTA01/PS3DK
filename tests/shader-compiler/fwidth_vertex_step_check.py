"""fwidth, and step in a vertex program (fwidth-vertex-step).

fwidth was not declared at all, and step() was lowered for fragment programs
only, so a vertex program that used it was refused.

fwidth(x), measured on the established compiler, is DDX, DDY and one MAX of
the two magnitudes: the larger of |ddx(x)| and |ddy(x)|.  The evaluator has
no derivative model (a derivative needs the neighbouring pixel), so fwidth is
judged against its explicit spelling, which already compiled: the two
containers must be byte-identical, and the container must hold a DDX, a DDY
and a MAX whose two operands are both taken by magnitude.  A function the
shader itself names fwidth is still called as written (judged by value).

step(edge, x) in a vertex program is one SGE x, edge.  There is no vertex
evaluator; the container must hold exactly one SGE per step, no other
comparison, and each form (scalar, vector, scalar edge against a vector)
must compile.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import fp_sources
import vp_words

FP_HEAD = 'float4 main(float4 p : TEXCOORD0) : COLOR {\n'
VP_HEAD = ('void main(float4 pos : POSITION, float4 uv : TEXCOORD0,\n'
           '          out float4 o : POSITION, out float4 c : COLOR) {\n o = pos;\n')

# name -> (fwidth spelling, explicit spelling)
FWIDTH = {
    'scalar': (' float w = fwidth(p.x);\n return float4(w, 0, 0, 1);\n}\n',
               ' float w = max(abs(ddx(p.x)), abs(ddy(p.x)));\n return float4(w, 0, 0, 1);\n}\n'),
    'float2': (' float2 w = fwidth(p.xy);\n return float4(w, 0, 1);\n}\n',
               ' float2 w = max(abs(ddx(p.xy)), abs(ddy(p.xy)));\n return float4(w, 0, 1);\n}\n'),
    'float3': (' float3 w = fwidth(p.xyz * p.w);\n return float4(w, 1);\n}\n',
               ' float3 v = p.xyz * p.w;\n float3 w = max(abs(ddx(v)), abs(ddy(v)));\n return float4(w, 1);\n}\n'),
    'scaled': (' float w = fwidth(p.x) * 1.5;\n return float4(w, 0, 0, 1);\n}\n',
               ' float w = max(abs(ddx(p.x)), abs(ddy(p.x))) * 1.5;\n return float4(w, 0, 0, 1);\n}\n'),
}
USER_FWIDTH = ('float fwidth(float x) { return x * 2.0; }\n' + FP_HEAD +
               ' return float4(fwidth(p.x), 0, 0, 1);\n}\n')

# name -> (body, number of step calls)
STEP = {
    'scalar': (' float s = step(0.5, uv.x);\n c = float4(s, 0, 0, 1);\n}\n', 1),
    'two': (' c = float4(step(0.5, uv.x), step(uv.y, uv.z), 0, 1);\n}\n', 2),
    'vector': (' c = step(float4(0.25, 0.5, 0.75, 1.0), uv);\n}\n', 1),
    'scalar_edge': (' c = float4(step(0.5, uv.xyz), 1);\n}\n', 1),
}


def compile_blob(compiler, tmp, name, profile, source):
    src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
    src.write_text(source)
    run = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    if run.returncode != 0 or not dst.exists():
        text = [l for l in (run.stdout + run.stderr).strip().splitlines() if 'error' in l or 'nv40' in l]
        return None, (text or ['refused'])[0][:160]
    return dst.read_bytes(), ''


def fwidth_shape(blob):
    names = []
    for w, _ in fp_sources.instructions(fp_sources.ucode_words(blob)):
        if (w[2] >> 31) & 1:
            continue
        name = fp_sources.OPCODE_NAME.get((w[0] >> 24) & 0x3F)
        names.append(name)
        if name == 'MAX' and not (fp_sources.source(w, 1)['abs'] and fp_sources.source(w, 2)['abs']):
            return 'a MAX operand is not taken by magnitude'
    for need in ('DDX', 'DDY', 'MAX'):
        if need not in names:
            return 'no %s' % need
    if 'ADD' in names:
        return 'an ADD: the magnitudes are summed'
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures = []
    tests = 0
    with tempfile.TemporaryDirectory(prefix='fwidth-step-') as tmp:
        for name, (short, explicit) in FWIDTH.items():
            tests += 1
            a, why = compile_blob(args.compiler, tmp, 'fw_' + name, 'sce_fp_rsx', FP_HEAD + short)
            b, why2 = compile_blob(args.compiler, tmp, 'ex_' + name, 'sce_fp_rsx', FP_HEAD + explicit)
            if a is None or b is None:
                failures.append('fwidth %s refused: %s' % (name, why or why2))
                continue
            if a != b:
                failures.append('fwidth %s: differs from max(abs(ddx), abs(ddy))' % name)
                continue
            if name != 'scaled':
                bad = fwidth_shape(a)
                if bad:
                    failures.append('fwidth %s: %s' % (name, bad))

        tests += 1
        blob, why = compile_blob(args.compiler, tmp, 'user_fwidth', 'sce_fp_rsx', USER_FWIDTH)
        if blob is None:
            failures.append('user fwidth refused: ' + why)
        else:
            for x in (0.25, 1.75, -3.5):
                got = list(fp_eval.evaluate(blob, {'TEX0': [x, 0.0, 0.0, 1.0]}))[:1]
                if got != [x * 2.0]:
                    failures.append('user fwidth: x=%s got %s want %s' % (x, got, [x * 2.0]))
                    break

        for name, (body, steps) in STEP.items():
            tests += 1
            blob, why = compile_blob(args.compiler, tmp, 'step_' + name, 'sce_vp_rsx', VP_HEAD + body)
            if blob is None:
                failures.append('vertex step %s refused: %s' % (name, why))
                continue
            lines, bad = vp_words.decode(blob)
            if lines is None:
                failures.append('vertex step %s: %s' % (name, bad))
                continue
            compares = [l.split() for l in lines
                        if l.split()[1] in ('SGE', 'SLT', 'SGT', 'SLE', 'SEQ', 'SNE')]
            if [c[1] for c in compares] != ['SGE'] * steps:
                failures.append('vertex step %s: comparisons %s, want %d SGE'
                                % (name, [c[1] for c in compares], steps))
            elif name == 'scalar' and not (compares[0][4].startswith('src0=IN') and
                                           compares[0][5].startswith('src1=C')):
                # step(0.5, uv.x) is SGE uv.x, 0.5: the value first, the edge second
                failures.append('vertex step scalar: operands %s %s, want the input then the constant'
                                % (compares[0][4], compares[0][5]))
    for failure in failures:
        print('FAIL:', failure)
    print('fwidth and vertex step: tests=%d fail=%d' % (tests, len(failures)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
