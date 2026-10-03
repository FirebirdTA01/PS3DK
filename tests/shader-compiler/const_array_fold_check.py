"""A static const file-scope array read at a constant index is its element.

`static const float k[3] = {...}; k[1]` was a uniform load of the const
global, and the lowering - no uniform record behind a const - refused it.
The reference emits the element inline.  Rows (review: codex): scalar,
vector, int, half and matrix element types, and shadowing (a local array
and a helper parameter named like the global), judged by value with fp_eval
against the reference-measured formula.  A NON-static `const` array is a
uniform with a default on the reference (it lists k[0..2] as parameters) -
the separate t_528b9869 gap - so it must keep refusing rather than fold.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

ELEMENT_TYPES = """static const float  kf[3] = {0.25, 0.5, 0.125};
static const float2 kv[2] = {float2(1.0, 2.0), float2(3.0, 4.0)};
static const int    ki[3] = {1, 2, 3};
static const half   kh[2] = {0.5, 0.75};
static const float2x2 km[2] = {float2x2(1, 2, 3, 4), float2x2(5, 6, 7, 8)};
float4 main(float4 t : TEXCOORD0) : COLOR
{
    return float4(kf[1] * t.x + kv[1].y, float(ki[2]) + kh[1], km[1][0].y, km[0][1].x);
}
"""

SHADOWED = """static const float k[3] = {0.25, 0.5, 0.125};
float g(float k[3]) { return k[1]; }
float4 main(float4 t : TEXCOORD0) : COLOR
{
    float k2 = k[2];
    {
        float k[3] = {t.x, t.y, t.z};
        return float4(k[1], k2, g(k), 1.0);
    }
}
"""

NON_STATIC = """const float k[3] = {0.25, 0.5, 0.125};
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(k[1] * t.x, k[2], 0, 1); }
"""

ROWS = [  # measured on the reference: (0.5 t.x + 4, 3.75, 6, 3) and (t.y, 0.125, t.y, 1)
    ('element_types', ELEMENT_TYPES, lambda t: [0.5 * t[0] + 4.0, 3.75, 6.0, 3.0]),
    ('shadowed', SHADOWED, lambda t: [t[1], 0.125, t[1], 1.0]),
]
INPUTS = [[0.75, 0.5, 0.25, 1.0], [0.25, -0.125, 0.5, 0.0]]


def compile_one(compiler, tmp, name, source):
    src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
    src.write_text(source)
    run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run, dst


def main():
    compiler = sys.argv[1]
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='const-array-fold-') as tmp:
        for name, source, formula in ROWS:
            run, dst = compile_one(compiler, tmp, name, source)
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[0]))
                print('  %-14s REFUSED' % name)
                continue
            blob = dst.read_bytes()
            for t in INPUTS:
                got, want = fp_eval.evaluate(blob, {'TEX0': t}), formula(t)
                ok = got == want
                print('  %-14s %-26s %s' % (name, t, 'value ok' if ok else 'WRONG %s want %s' % (got, want)))
                if not ok:
                    failures.append('%s: got %s for %s, want %s' % (name, got, t, want))
        run, dst = compile_one(compiler, tmp, 'non_static', NON_STATIC)
        ok = run.returncode == 1 and not dst.exists()
        print('  %-14s %s' % ('non_static', 'refused (t_528b9869)' if ok else 'NOT refused (rc %d)' % run.returncode))
        if not ok:
            failures.append('non_static const array must keep refusing (t_528b9869), rc %d' % run.returncode)
    for f in failures:
        print('FAIL:', f)
    print('const-array-fold: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
