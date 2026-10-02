"""fmod() and all() are lowered, not left as calls (community bucket).

Measured on sce-cgc 475: fmod(a, b) is r = frac(|a / b|) * |b|, negated
where a < 0 (RCP+MUL per lane or DIV, FRC of the absolute quotient, MUL by
|b|, a predicated negate on a's sign); all(v) is every lane != 0 (SNE per
lane, multiplied into the condition register).  33 community programs
(crt-cgwg-fast and the rest) were refused with "unsupported IR op call".

Inputs are exact in float32 at every step (dyadic dividends, power-of-two
divisors), so the expected value is the formula itself, signs both ways.
Vertex rows are judged by value with the VP evaluator; a vertex vector fmod
and two vector operands of different widths stay refused by name.
"""
import argparse
import itertools
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval

A = [-2.75, -0.625, -0.0, 0.375, 1.5, 3.125]
B = [0.25, 0.5, 2.0, 4.0, -0.5, -2.0]


def fmod_ref(a, b):
    r = (abs(a / b) % 1.0) * abs(b)
    return -r if a < 0 else r


FP_ROWS = {
    'fmod_vector': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return fmod(a, b); }\n',
                    lambda a, b: [fmod_ref(x, y) for x, y in zip(a, b)]),
    'fmod_scalar': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return float4(fmod(a.x, b.x), a.y, b.y, 1); }\n',
                    lambda a, b: [fmod_ref(a[0], b[0]), a[1], b[1], 1.0]),
    'all_numeric': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return all(a.xy) ? b : -b; }\n',
                    lambda a, b: list(b) if a[0] != 0 and a[1] != 0 else [-v for v in b]),
    'all_compare': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { bool3 c = a.xyz > 0.0; return all(c) ? a : b; }\n',
                    lambda a, b: list(a) if a[0] > 0 and a[1] > 0 and a[2] > 0 else list(b)),
    # a scalar operand broadcasts (measured on the reference)
    'fmod_vec_by_scalar': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return fmod(a, b.x); }\n',
                           lambda a, b: [fmod_ref(x, b[0]) for x in a]),
    'fmod_scalar_by_vec': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return fmod(a.x, b); }\n',
                           lambda a, b: [fmod_ref(a[0], y) for y in b]),
    'any_still': ('float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR { return any(a.xy) ? b : -b; }\n',
                  lambda a, b: list(b) if a[0] != 0 or a[1] != 0 else [-v for v in b]),
}
# Vertex rows are judged by value with the VP evaluator (binary32).  A
# vector fmod needs a per-lane select, which the vertex path does not have
# (t_ca8f99a6): it stays refused by name although the reference accepts it.
VP_ROWS = {
    'fmod_scalar_vp': ('float4 main(float4 p : POSITION, uniform float d) : POSITION { return float4(fmod(p.x, d), p.yzw); }\n',
                       lambda p: [fmod_ref(p[0], 0.5), p[1], p[2], p[3]]),
    'all_vp': ('float4 main(float4 p : POSITION) : POSITION { return all(p.xyz) ? p : p * 2; }\n',
               lambda p: list(p) if p[0] != 0 and p[1] != 0 and p[2] != 0 else [2 * x for x in p]),
}
VP_REFUSE = {
    'fmod_vector_vp': 'float4 main(float4 p : POSITION, uniform float4 d) : POSITION { return fmod(p, d); }\n',
}
VP_GRID = [[-2.75, 1.5, 0.375, 3.125], [0.375, -0.5, 2.0, 1.0], [-0.625, 1.5, 0.0, 3.125], [3.125, -2.75, 1.5, -0.0]]


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
    grid = []
    for i, (x, y) in enumerate(itertools.product(A, B)):
        a = [x, A[(i + 1) % len(A)], A[(i + 2) % len(A)], A[(i + 3) % len(A)]]
        b = [y, B[(i + 1) % len(B)], B[(i + 2) % len(B)], B[(i + 3) % len(B)]]
        grid.append((a, b))
    with tempfile.TemporaryDirectory(prefix='fmod-all-') as tmp:
        work = Path(tmp)
        for name, (text, want) in FP_ROWS.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_fp_rsx')
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-14s REFUSED' % name)
                continue
            bad = []
            for a, b in grid:
                got = fp_eval.evaluate(blob, {'TEX0': a, 'TEX1': b})
                exp = want(a, b)
                if any(not (g == e or (g == 0.0 and e == 0.0)) or math.copysign(1, g) != math.copysign(1, e)
                       for g, e in zip(got, exp)):
                    bad.append((a, b, got, exp))
            print('  %-14s %s' % (name, 'values ok (%d inputs)' % len(grid) if not bad else 'WRONG on %d' % len(bad)))
            if bad:
                failures.append('%s: got %s for %s, want %s' % (name, bad[0][2], bad[0][:2], bad[0][3]))
        for name, (text, want) in VP_ROWS.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_vp_rsx')
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-14s REFUSED' % name)
                continue
            bad = [p for p in VP_GRID
                   if vp_eval.evaluate(blob, {'d': [0.5, 0.0, 0.0, 0.0]}, inputs={0: p}, binary32=True).get(0) != want(p)]
            print('  %-14s %s' % (name, 'values ok (%d inputs)' % len(VP_GRID) if not bad else 'WRONG on %d' % len(bad)))
            if bad:
                failures.append('%s: got %s for %s, want %s' % (
                    name, vp_eval.evaluate(blob, {'d': [0.5, 0.0, 0.0, 0.0]}, inputs={0: bad[0]}, binary32=True).get(0),
                    bad[0], want(bad[0])))
        for name, text in VP_REFUSE.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_vp_rsx')
            ok = rc == 1 and not blob and 'vertex select with a vector condition' in err
            print('  %-14s %s' % (name, 'refused by name' if ok else 'NOT refused by name (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected the named VP select refusal, got rc %d' % (name, rc))
    for f in failures:
        print('FAIL:', f)
    print('fmod-all: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
