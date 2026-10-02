"""A helper's out / inout parameters are copy-out (t_a290c3c8 follow-up).

libretro's quad-pixel-communication.h quad_gather(..., out float4 adjx, out
float4 adjy, out float4 diag) is called from helpers on uninitialised locals,
and 16 community shaders were refused ("out/inout parameters are not
supported").  The reference accepts every row below; each expected value was
measured on it (sce-cgc 475) and is written as the Cg expression.  Aliasing:
two(x, x) leaves the LEFTMOST parameter's value (x = 1).

Still refused by name: an out parameter of struct type (the reference accepts
it - a named gap, not a silent drop).
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval

ROWS = {
    'split': ("""void split(float4 c, out float4 a, out float4 b) { a = c * 2.0; b = c * 3.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 x, y; split(t, x, y); return x + y * 10.0; }
""", lambda t: [v * 32.0 for v in t]),
    'inout_chain': ("""void scale(inout float4 v, float k) { v = v * k; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 r = t; scale(r, 2.0); scale(r, 3.0); return r; }
""", lambda t: [v * 6.0 for v in t]),
    'swizzle_dest': ("""void set2(out float2 o) { o = float2(0.25, 0.75); }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 r = t; set2(r.yw); return r; }
""", lambda t: [t[0], 0.25, t[2], 0.75]),
    'entry_out': ("""void fill(float4 t, out float4 o) { o = t * 0.5; }
void main(float4 t : TEXCOORD0, out float4 c : COLOR) { fill(t, c); }
""", lambda t: [v * 0.5 for v in t]),
    'nested': ("""void inner(float4 c, out float4 a, out float4 b) { a = c + 1.0; b = a * 2.0; }
float4 outer(float4 c) { float4 p, q; inner(c, p, q); return p + q; }
float4 main(float4 t : TEXCOORD0) : COLOR { return outer(t) * 0.5; }
""", lambda t: [(v + 1.0) * 1.5 for v in t]),
    'alias': ("""void two(out float4 a, out float4 b) { a = float4(1, 1, 1, 1); b = float4(2, 2, 2, 2); }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 x; two(x, x); return x * t; }
""", lambda t: list(t)),
    'with_return': ("""float firsthalf(float4 c, out float4 rest) { rest = c * 4.0; return c.x * 0.5; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 r; float h = firsthalf(t, r); return r + h; }
""", lambda t: [v * 4.0 + t[0] * 0.5 for v in t]),
    'inout_member': ("""struct S { float4 v; };
void bump(inout float4 v) { v = v + 1.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { S s; s.v = t; bump(s.v); return s.v; }
""", lambda t: [v + 1.0 for v in t]),
}
REFUSE = {
    'struct_out': """struct P { float4 a; float4 b; };
void mk(float4 c, out P p) { p.a = c; p.b = c * 2.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { P p; mk(t, p); return p.a + p.b; }
""",
}
# Vertex helpers copy out too; judged by value with the VP evaluator
# (binary32; dyadic inputs keep every product exact).  vp_split is the
# control-flow test's vp_inline_void_out_v.
VP_ROWS = {
    'vp_split': ('void split(float4 p, out float3 a, out float w) { a = p.xyz; w = p.w; } void main(float4 p : POSITION, out float4 o : POSITION) { float3 a; float w; split(p, a, w); o = float4(a * w, w); }\n',
                 lambda p: [p[0] * p[3], p[1] * p[3], p[2] * p[3], p[3]]),
    'vp_inout': ('void scale(inout float4 v, float k) { v = v * k; } float4 main(float4 p : POSITION) : POSITION { float4 v = p; scale(v, 2.0); scale(v, p.w); return v; }\n',
                 lambda p: [x * 2.0 * p[3] for x in p]),
}
GRID = [[0.5, -0.25, 1.0, 0.75], [-1.0, 0.0, 0.25, 2.0], [0.125, 0.5, -0.5, 1.0]]


def compile_one(compiler, work, name, text, profile='sce_fp_rsx'):
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
    with tempfile.TemporaryDirectory(prefix='inline-out-param-') as tmp:
        work = Path(tmp)
        for name, (text, want) in ROWS.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-14s REFUSED' % name)
                continue
            bad = [t for t in GRID if fp_eval.evaluate(blob, {'TEX0': t}) != want(t)]
            print('  %-14s %s' % (name, 'values ok' if not bad else 'WRONG on %d inputs' % len(bad)))
            if bad:
                failures.append('%s: got %s for %s, want %s' % (
                    name, fp_eval.evaluate(blob, {'TEX0': bad[0]}), bad[0], want(bad[0])))
        for name, (text, want) in VP_ROWS.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_vp_rsx')
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-14s REFUSED' % name)
                continue
            got = [vp_eval.evaluate(blob, {}, inputs={0: t}, binary32=True).get(0) for t in GRID]
            bad = [(t, g) for t, g in zip(GRID, got) if g != want(t)]
            print('  %-14s %s' % (name, 'values ok' if not bad else 'WRONG on %d inputs' % len(bad)))
            if bad:
                failures.append('%s: got %s for %s, want %s' % (name, bad[0][1], bad[0][0], want(bad[0][0])))
        for name, text in REFUSE.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            ok = rc == 1 and not blob and 'out/inout parameter' in err
            print('  %-14s %s' % (name, 'refused by name' if ok else 'NOT refused by name (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected the named refusal, got rc %d' % (name, rc))
    for f in failures:
        print('FAIL:', f)
    print('inline-out-param: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
