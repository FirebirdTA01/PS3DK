"""A file-scope const matrix folds to literal rows (community xbr family).

libretro's xbr/super-xbr shaders declare `const static float3x3
yuv_weighted = float3x3(...)` and multiply by it; the folded constant
reached the backend as an IRConstant that no matrix lowering recognised
("matvecmul matrix source is not a matrix value"), about ten programs.
The reference accepts every row below.  Entries are distinct dyadic values,
so every product is exact and a transposed, shuffled or zeroed row gives a
different answer: each expected value is the matrix product computed here
(Cg: float3x3(a..i) is rows abc, def, ghi; mul(M, v) is M v, mul(v, M) is
v M).
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval

W3 = [[0.5, -1.25, 2.0], [0.75, 3.0, -0.5], [-2.0, 0.25, 1.5]]
W4 = [[1.0, 0.5, -0.25, 2.0], [0.0, -1.5, 0.75, 1.0], [2.5, 0.125, 1.0, -1.0], [-0.5, 1.0, 0.25, 0.5]]
W2 = [[0.5, 1.5], [-2.0, 0.25]]
W43 = [[0.5, 1.0, -1.5], [2.0, -0.25, 0.75], [1.25, 0.5, -1.0], [-0.5, 2.0, 0.25]]


def lit(m, name, decl):
    return 'const static %s %s = %s(%s);\n' % (decl, name, decl, ', '.join(repr(x) for row in m for x in row))


def mv(m, v):
    return [sum(a * b for a, b in zip(row, v)) for row in m]


def vm(v, m):
    return [sum(v[i] * m[i][j] for i in range(len(m))) for j in range(len(m[0]))]


T = [[0.5, -0.25, 1.0, 0.75], [-1.0, 2.0, 0.25, -0.5], [1.5, 0.125, -0.75, 2.0]]
FP_ROWS = {
    'mul_m3_v': (lit(W3, 'W', 'float3x3') + 'float4 main(float4 t : TEXCOORD0) : COLOR { return float4(mul(W, t.xyz), 1); }\n',
                 lambda t: mv(W3, t[:3]) + [1.0]),
    'mul_v_m3': (lit(W3, 'W', 'float3x3') + 'float4 main(float4 t : TEXCOORD0) : COLOR { return float4(mul(t.xyz, W), 1); }\n',
                 lambda t: vm(t[:3], W3) + [1.0]),
    'mul_m4_v': (lit(W4, 'W', 'float4x4') + 'float4 main(float4 t : TEXCOORD0) : COLOR { return mul(W, t); }\n',
                 lambda t: mv(W4, t)),
    'mul_m2_v': (lit(W2, 'W', 'float2x2') + 'float4 main(float4 t : TEXCOORD0) : COLOR { return float4(mul(W, t.xy), t.zw); }\n',
                 lambda t: mv(W2, t[:2]) + t[2:]),
    'mul_m43_v': (lit(W43, 'W', 'float4x3') + 'float4 main(float4 t : TEXCOORD0) : COLOR { return mul(W, t.xyz); }\n',
                  lambda t: mv(W43, t[:3])),
    'const_static_order': ('static const float3x3 W = float3x3(' + ', '.join(repr(x) for r in W3 for x in r) +
                           ');\nfloat4 main(float4 t : TEXCOORD0) : COLOR { return float4(mul(W, t.xyz), 1); }\n',
                           lambda t: mv(W3, t[:3]) + [1.0]),
    'in_helper': (lit(W3, 'W', 'float3x3') + 'float3 f(float3 v) { return abs(mul(W, v)); }\n'
                  'float4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t.xyz), 1); }\n',
                  lambda t: [abs(x) for x in mv(W3, t[:3])] + [1.0]),
}
VP_ROWS = {
    'vp_mul_m3_v': (lit(W3, 'W', 'float3x3') + 'float4 main(float4 p : POSITION) : POSITION { return float4(mul(W, p.xyz), 1); }\n',
                    lambda p: mv(W3, p[:3]) + [1.0]),
    'vp_mul_m4_v': (lit(W4, 'W', 'float4x4') + 'float4 main(float4 p : POSITION) : POSITION { return mul(W, p); }\n',
                    lambda p: mv(W4, p)),
}


def compile_one(compiler, work, name, text, profile):
    src, dst = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='static-const-matrix-') as tmp:
        work = Path(tmp)
        for rows, profile in ((FP_ROWS, 'sce_fp_rsx'), (VP_ROWS, 'sce_vp_rsx')):
            for name, (text, want) in rows.items():
                rc, blob, err = compile_one(args.compiler, work, name, text, profile)
                if rc != 0 or not blob:
                    failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                    print('  %-18s REFUSED' % name)
                    continue
                if profile == 'sce_fp_rsx':
                    run = lambda t: fp_eval.evaluate(blob, {'TEX0': t})
                else:
                    run = lambda t: vp_eval.evaluate(blob, {}, inputs={0: t}, binary32=True).get(0)
                bad = [(t, run(t)) for t in T if run(t) != want(t)]
                print('  %-18s %s' % (name, 'values ok' if not bad else 'WRONG on %d inputs' % len(bad)))
                if bad:
                    failures.append('%s: got %s for %s, want %s' % (name, bad[0][1], bad[0][0], want(bad[0][0])))
    for f in failures:
        print('FAIL:', f)
    print('static-const-matrix: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
