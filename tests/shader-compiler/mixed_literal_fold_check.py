"""Float and integer literals fold together in fragment programs (fold-mixed-literals).

`1.5f * 4.0f` was folded to one constant, `1.5f * 4` was not: the integer
literal was converted and the product computed by the program, a MOV and a
MUL for every such term.  A colour-conversion matrix written as
`1.8556f * 255 / 224` cost 55 instructions where the established compiler
needs 11.  An operation on a float constant and an integer constant folds
now, the integer promoted to float as the run-time conversion would.  Two
integer literals stay integer arithmetic (`7 / 2` is 3).

A matrix whose rows are constants is read as inline constants: its rows are
no longer copied into registers a lane at a time (the `matrix` row counts the
MOVs that load a constant).  Values are compared bit for bit, so a -0.0 that
became +0.0 is a failure.

Every row is judged by VALUE with the evaluator against a plain Python
rendering, and by SHAPE: the number of instructions other than MOV, which is
the one multiply per input lane group when the constant arithmetic is gone.  Constants are dyadic so every expected value is
exact in binary32.  TEXCOORD0.xy supplies the operands.
"""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import fp_sources

HEAD = 'float4 main(float2 p : TEXCOORD0) : COLOR {\n'

# name -> (source, expected(TEX0.x, TEX0.y) -> [COLOR.x, COLOR.y], instructions other than MOV)
ROWS = {
    'float_times_int': (HEAD + ' float k = 1.5f * 4;\n return float4(p.x * k, 0, 0, 1);\n}\n',
                        lambda x, y: [x * 6.0, 0.0], 1),
    'int_times_float': (HEAD + ' float k = 4 * 1.5f;\n return float4(p.x * k, 0, 0, 1);\n}\n',
                        lambda x, y: [x * 6.0, 0.0], 1),
    'chain': (HEAD + ' float k = 1.5f * 6 / 4 + 3 - 0.25f;\n return float4(p.x * k, 0, 0, 1);\n}\n',
              lambda x, y: [x * 5.0, 0.0], 1),
    'divide': (HEAD + ' float k = 3.0f / 4;\n float j = 3 / 4.0f;\n return float4(p.x * k, p.y * j, 0, 1);\n}\n',
               lambda x, y: [x * 0.75, y * 0.75], 2),
    # two integers divide as integers before the float joins
    'int_pair_first': (HEAD + ' float k = 7 / 2 * 0.5f;\n return float4(p.x * k, 0, 0, 1);\n}\n',
                       lambda x, y: [x * 1.5, 0.0], 1),
    'vector': (HEAD + ' float2 k = float2(0.5f, 1.5f) * 4;\n return float4(p * k, 0, 1);\n}\n',
               lambda x, y: [x * 2.0, y * 6.0], 1),
    'matrix': (HEAD + ' const float2x2 k = { 1.5f * 2, 0.5, 0.25, 3 / 4.0f };\n'
               ' return float4(mul(k, p), 0, 1);\n}\n',
               lambda x, y: [3.0 * x + 0.5 * y, 0.25 * x + 0.75 * y], 2),
    # a row holding both zeros keeps each lane's sign (it stays lane by lane)
    'matrix_zero_signs': (HEAD + ' float2x4 m = float2x4(0.0, 1.0, -0.0, 2.0, p.x, p.y, 3.0, 4.0);\n'
                          ' return float4(m[0].xz, m[1].xy);\n}\n',
                          lambda x, y: [0.0, -0.0], 0),
    'matrix_zero_signs_reversed': (HEAD + ' float2x4 m = float2x4(-0.0, 1.0, 0.0, 2.0, p.x, p.y, 3.0, 4.0);\n'
                                   ' return float4(m[0].xz, m[1].xy);\n}\n',
                                   lambda x, y: [-0.0, 0.0], 0),
    # a constant row read through a product, a transpose and a row index
    'matrix_transpose': (HEAD + ' float2x2 m = float2x2(1.0, 2.0, p.x, p.y);\n'
                         ' return float4(mul(transpose(m), float2(1.0, 2.0)), 0, 1);\n}\n',
                         lambda x, y: [1.0 + 2.0 * x, 2.0 + 2.0 * y], 4),
    'matrix_row_index': (HEAD + ' float2x2 m = float2x2(1.5, 2.5, p.x, p.y);\n'
                         ' return float4(m[0].yx, m[1]);\n}\n',
                         lambda x, y: [2.5, 1.5], 0),
}
# row -> most MOVs that may load a constant (the result's literal lanes)
CONSTANT_MOVS = {'matrix': 2}
VECTORS = [(0.25, 0.5), (1.75, 2.25), (3.5, 0.75), (-1.25, 4.5), (2.0, 3.0), (5.75, -2.25)]


def bits(values):
    return [struct.pack('>f', v) for v in values]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='mixed-literal-fold-') as tmp:
        for name, (source, expected, count) in ROWS.items():
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([args.compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                text = (run.stdout + run.stderr).strip().splitlines()
                failures.append('%s refused: %s' % (name, (text or ['?'])[0]))
                continue
            blob = dst.read_bytes()
            before = len(failures)
            for x, y in VECTORS:
                want = expected(x, y)
                try:
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0]}))[:2]
                except Exception as error:          # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if bits(got) != bits(want):          # by bits: -0.0 == 0.0 as floats
                    failures.append('%s: p=(%s, %s) got %s want %s' % (name, x, y, got, want))
                    break
            if len(failures) == before:
                got_count = sum(1 for w, _ in fp_sources.instructions(fp_sources.ucode_words(blob))
                                if (w[2] >> 31) & 1 or (w[0] >> 24) & 0x3F != 0x01)
                if got_count > count:
                    failures.append('%s: %d instructions other than MOV, at most %d when the constants fold'
                                    % (name, got_count, count))
                loads = sum(1 for w, _ in fp_sources.instructions(fp_sources.ucode_words(blob))
                            if not (w[2] >> 31) & 1 and (w[0] >> 24) & 0x3F == 0x01
                            and fp_sources.source(w, 1)['type'] == fp_sources.CONST)
                if loads > CONSTANT_MOVS.get(name, loads):
                    failures.append('%s: %d MOVs load a constant, at most %d when the rows are inline'
                                    % (name, loads, CONSTANT_MOVS[name]))
    for failure in failures:
        print('FAIL:', failure)
    print('mixed literal fold: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
