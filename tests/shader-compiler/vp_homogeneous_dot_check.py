"""Homogeneous dot products and direct output writes in vertex programs (vp-dph-direct-outputs).

`mul(M, float4(p, 1.0))` built the float4 in a register and took four DP4s of
it, and its result went to a register and was copied to the output: twelve
instructions for the usual two-matrix position transform where the
established compiler needs nine.  A dot product with a vector whose w is the
literal 1 is now one DPH (a.xyz . b.xyz + b.w), and a value computed into a
register only to be copied whole to an output is computed into the output.

Every row is judged by VALUE with the vertex evaluator against a plain Python
rendering (dyadic inputs and uniforms, so the expected values are exact), and
by SHAPE: the number of DPH and DP4 instructions and of register-to-output
copies.  The rows that must NOT fold (a w that is not 1, a float4 whose w is
read elsewhere, a negated homogeneous vector, a register copied to two
outputs) are judged the same way.

Inputs: POSITION (register 0) and TEXCOORD0 (register 8).  Outputs: POSITION
(0), COLOR0 (1), TEXCOORD0 (7).  The matrices are built from float4 uniforms
m0..m3 and n0..n3, one per row.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import vp_words
from vp_pow_vector_check import evaluate

HEAD = ('uniform float4 m0, m1, m2, m3, n0, n1, n2, n3;\n'
        'void main(float4 p : POSITION, float4 uv : TEXCOORD0,\n'
        '          out float4 o : POSITION, out float4 c : COLOR0, out float4 t : TEXCOORD0) {\n'
        ' float4x4 M = float4x4(m0, m1, m2, m3);\n float4x4 N = float4x4(n0, n1, n2, n3);\n')
RETURN_HEAD = ('uniform float4 m0, m1, m2, m3, n0, n1, n2, n3;\n'
               'float4 main(float4 p : POSITION, float4 uv : TEXCOORD0, out float4 t : TEXCOORD0) : POSITION {\n'
               ' float4x4 M = float4x4(m0, m1, m2, m3);\n float4x4 N = float4x4(n0, n1, n2, n3);\n')
UNIFORMS = {
    'm0': [1.0, 0.5, 0.25, 2.0], 'm1': [-0.5, 1.5, 0.0, -1.0], 'm2': [0.25, 0.0, 2.0, 0.5], 'm3': [0.0, 0.5, -1.0, 4.0],
    'n0': [2.0, 0.0, 0.5, 1.0], 'n1': [0.0, -1.0, 0.25, 0.5], 'n2': [1.0, 1.0, 0.0, -2.0], 'n3': [0.5, 0.0, 0.0, 1.0],
}
M = [UNIFORMS['m%d' % i] for i in range(4)]
N = [UNIFORMS['n%d' % i] for i in range(4)]
VECTORS = [([0.25, 0.5, 0.75, 3.0], [0.5, 0.25, 2.0, 1.0]), ([-1.5, 2.0, 0.5, 0.5], [1.0, 0.0, -0.5, 0.25]),
           ([4.0, -0.25, 1.25, -2.0], [0.0, 3.0, 0.5, 2.0])]


def dot4(a, b):
    return sum(a[k] * b[k] for k in range(4))


def mul(rows, v):
    return [sum(r[k] * v[k] for k in range(4)) for r in rows]


# name -> (body, expected(p, uv) -> {output: value}, DPH count, DP4 count, register-to-output copies)
ROWS = {
    'position': (' o = mul(M, float4(p.xyz, 1.0));\n c = uv;\n t = uv;\n}\n',
                 lambda p, uv: {0: mul(M, p[:3] + [1.0]), 1: uv, 7: uv}, 4, 0, 0),
    # the usual world then view-projection transform
    'two_matrices': (' float4 world = mul(M, float4(p.xyz, 1.0));\n o = mul(N, world);\n c = uv;\n t = uv;\n}\n',
                     lambda p, uv: {0: mul(N, mul(M, p[:3] + [1.0])), 1: uv, 7: uv}, 4, 4, 0),
    # the float4's w is read by something else: the 1.0 has to exist
    'w_read_elsewhere': (' float4 h = float4(p.xyz, 1.0);\n o = mul(M, h);\n c = h;\n t = uv;\n}\n',
                         lambda p, uv: {0: mul(M, p[:3] + [1.0]), 1: p[:3] + [1.0], 7: uv}, None, None, None),
    # a w that is not 1 is an ordinary four-lane dot product
    'w_not_one': (' o = mul(M, float4(p.xyz, 2.0));\n c = uv;\n t = uv;\n}\n',
                  lambda p, uv: {0: mul(M, p[:3] + [2.0]), 1: uv, 7: uv}, 0, 4, 0),
    # a NEGATED homogeneous vector is not homogeneous: its w is -1, so the
    # row's w is subtracted.  Both operand orders stay four-lane dot products.
    'negated': (' float4 h = float4(p.xyz, 1.0);\n o = float4(dot(-h, m0), dot(m1, -h), dot(-h, n0), 1.0);\n'
                ' c = uv;\n t = uv;\n}\n',
                lambda p, uv: {0: [-dot4(p[:3] + [1.0], M[0]), -dot4(p[:3] + [1.0], M[1]),
                                   -dot4(p[:3] + [1.0], N[0]), 1.0], 1: uv, 7: uv}, 0, None, None),
    # a row computed by the program, not a constant
    'computed_row': (' float4 r = uv * m0 + n0;\n float4 h = float4(p.xyz, 1.0);\n'
                     ' o = float4(dot(h, r), dot(r, h), dot(-h, r), 1.0);\n c = uv;\n t = uv;\n}\n',
                     lambda p, uv: (lambda r, h: {0: [dot4(h, r), dot4(r, h), -dot4(h, r), 1.0], 1: uv, 7: uv})(
                         [uv[k] * M[0][k] + N[0][k] for k in range(4)], p[:3] + [1.0]), None, None, None),
    'w_from_input': (' o = mul(M, p);\n c = uv;\n t = uv;\n}\n',
                     lambda p, uv: {0: mul(M, p), 1: uv, 7: uv}, 0, 4, 0),
    # one computed value copied to two outputs keeps its register
    'two_outputs': (' float4 v = mul(N, uv);\n o = p;\n c = v;\n t = v;\n}\n',
                    lambda p, uv: {0: p, 1: mul(N, uv), 7: mul(N, uv)}, 0, 4, None),
    # the result is the function's return value (the copy this row used to keep)
    'returned': (RETURN_HEAD + ' float4 world = mul(M, float4(p.xyz, 1.0));\n t = uv;\n'
                 ' return mul(N, world);\n}\n',
                 lambda p, uv: {0: mul(N, mul(M, p[:3] + [1.0])), 7: uv}, 4, 4, 0),
    # scalars computed one by one and placed in output lanes: each dot product
    # writes its output lane, no register in between
    'lane_scatter': (' o = p;\n c = float4(dot(p.xyz, m0.xyz), dot(uv.xyz, m1.xyz), dot(p, m2), 1.0);\n'
                     ' t = float4(uv.xy, dot(uv, n0), dot(p, n1));\n}\n',
                     lambda p, uv: {0: p, 1: [sum(p[k] * M[0][k] for k in range(3)),
                                              sum(uv[k] * M[1][k] for k in range(3)), dot4(p, M[2]), 1.0],
                                    7: [uv[0], uv[1], dot4(uv, N[0]), dot4(p, N[1])]}, 0, None, None),
    # a lane-wise value copied to the output in another lane order
    'reordered': (' o = p;\n c = (uv * m0 + n0).zyxw;\n t = uv;\n}\n',
                  lambda p, uv: (lambda v: {0: p, 1: [v[2], v[1], v[0], v[3]], 7: uv})(
                      [uv[k] * M[0][k] + N[0][k] for k in range(4)]), 0, 0, None),
    # one lane repeated into three output lanes
    'repeated': (' o = p;\n float d = dot(uv, m0);\n c = float4(d, d, d, 1.0);\n t = uv;\n}\n',
                 lambda p, uv: {0: p, 1: [dot4(uv, M[0])] * 3 + [1.0], 7: uv}, 0, None, None),
    # arithmetic written straight to an output
    'arithmetic': (' o = p;\n c = uv * m0 + n0;\n t = uv;\n}\n',
                   lambda p, uv: {0: p, 1: [uv[k] * M[0][k] + N[0][k] for k in range(4)], 7: uv}, 0, 0, 0),
}


def shape(blob):
    lines, bad = vp_words.decode(blob)
    if lines is None:
        return None, bad
    parts = [l.split() for l in lines]
    copies = sum(1 for q in parts if q[1] == 'MOV' and q[2].startswith('dst=o') and q[4].startswith('src0=R'))
    return (sum(1 for q in parts if q[1] == 'DPH'), sum(1 for q in parts if q[1] == 'DP4'), copies), None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='vp-homogeneous-dot-') as tmp:
        for name, (body, expected, dph, dp4, copies) in ROWS.items():
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(body if body.startswith('uniform') else HEAD + body)
            run = subprocess.run([args.compiler, '-p', 'sce_vp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                text = [l for l in (run.stdout + run.stderr).strip().splitlines() if 'error' in l or 'nv40' in l]
                failures.append('%s refused: %s' % (name, (text or ['?'])[0][:160]))
                continue
            blob = dst.read_bytes()
            before = len(failures)
            for p, uv in VECTORS:
                want = expected(list(p), list(uv))
                try:
                    got = evaluate(blob, UNIFORMS, {0: list(p), 8: list(uv)})
                except AssertionError as error:     # an unmodelled or undefined read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                wrong = [k for k in want if got.get(k) != want[k]]
                if wrong:
                    k = wrong[0]
                    failures.append('%s: p=%s uv=%s output %d got %s want %s' % (name, p, uv, k, got.get(k), want[k]))
                    break
            if len(failures) != before:
                continue
            found, bad = shape(blob)
            if found is None:
                failures.append('%s: %s' % (name, bad))
                continue
            for label, got_n, want_n in (('DPH', found[0], dph), ('DP4', found[1], dp4),
                                         ('register-to-output copies', found[2], copies)):
                if want_n is not None and got_n != want_n:
                    failures.append('%s: %d %s, want %d' % (name, got_n, label, want_n))
    for failure in failures:
        print('FAIL:', failure)
    print('vp homogeneous dot: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len({f.split(':')[0].split(' ')[0] for f in failures}),
             len({f.split(':')[0].split(' ')[0] for f in failures}), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
