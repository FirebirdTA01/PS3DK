"""A vector stored into a scalar keeps lane x (community bucket, 19 programs).

Measured on sce-cgc 475: a declaration's initialiser or a plain `=` that
stores a numeric VECTOR into a numeric SCALAR is accepted with warning C7011
and keeps lane x, converted to the scalar's type (float a = t.xyz is t.x;
a = t.yz is t.y; half a = t.zw is t.z; int a = t.xy truncates t.x).
libretro's ddt-waterpaint, 2xbr-v3.x and oldtv were refused here with
"cannot assign 'floatN' to 'float'".  Each expected value is the reference's.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

T = [0.5, -0.25, 0.75, 1.5]
ROWS = {
    'decl_from_float3': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = t.xyz; return float4(a, 0, 0, 1); }\n',
                         [0.5, 0.0, 0.0, 1.0]),
    'decl_from_float4': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = t; return float4(a, 0, 0, 1); }\n',
                         [0.5, 0.0, 0.0, 1.0]),
    'assign_from_float2': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a; a = t.yz; return float4(a, 0, 0, 1); }\n',
                           [-0.25, 0.0, 0.0, 1.0]),
    'decl_half': ('float4 main(float4 t : TEXCOORD0) : COLOR { half a = t.zw; return float4(a, 0, 0, 1); }\n',
                  [0.75, 0.0, 0.0, 1.0]),
    'decl_int': ('float4 main(float4 t : TEXCOORD0) : COLOR { int a = t.xy; return float4(a, 0, 0, 1); }\n',
                 [0.0, 0.0, 0.0, 1.0]),
    'computed_value': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = t.yxw * 2.0 + 1.0; return float4(a, a, 0, 1); }\n',
                       [0.5, 0.5, 0.0, 1.0]),
    # compound assignment: a op= v is a op v.x (measured)
    'compound_mul': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = t.w; a *= t.yz; return float4(a, 0, 0, 1); }\n',
                     [-0.375, 0.0, 0.0, 1.0]),
    'compound_add': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = t.w; a += t; return float4(a, 0, 0, 1); }\n',
                     [2.0, 0.0, 0.0, 1.0]),
    'compound_self': ('float4 main(float4 t : TEXCOORD0) : COLOR { float a = t.x; a *= (a * t); return float4(a, 0, 0, 1); }\n',
                      [0.125, 0.0, 0.0, 1.0]),
    # the operation runs in v's element type and only then converts to a's
    # (review: codex): int a = 3; a *= float2(1.5, 9) is int(4.5) = 4, not 3 * 1
    'compound_int_mul': ('float4 main(float4 t : TEXCOORD0) : COLOR { int a = 3; a *= float2(1.5, 9); return float4(a, 0, 0, 1); }\n',
                         [4.0, 0.0, 0.0, 1.0]),
    'compound_int_div': ('float4 main(float4 t : TEXCOORD0) : COLOR { int a = 3; a /= float2(1.5, 9); return float4(a, 0, 0, 1); }\n',
                         [2.0, 0.0, 0.0, 1.0]),
    # a function declared to return a scalar keeps lane x of a vector
    'return_float': ('float f(float4 t) { return t.zyx; }\nfloat4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t), 0, 0, 1); }\n',
                     [0.75, 0.0, 0.0, 1.0]),
    'return_half': ('half f(float4 t) { return t.yz; }\nfloat4 main(float4 t : TEXCOORD0) : COLOR { return float4(f(t), 0, 0, 1); }\n',
                    [-0.25, 0.0, 0.0, 1.0]),
    # an int vector into a float scalar converts lane x
    'int_vector_to_float': ('float4 main(float4 t : TEXCOORD0) : COLOR { int2 i = int2(t.xz * 4.0); float a = i; return float4(a, 0, 0, 1); }\n',
                            [2.0, 0.0, 0.0, 1.0]),
    'int_vector_to_half': ('float4 main(float4 t : TEXCOORD0) : COLOR { int2 i = int2(t.xz * 4.0); half a = i; return float4(a, 0, 0, 1); }\n',
                           [2.0, 0.0, 0.0, 1.0]),
    # a declaration narrows a vector to a shorter vector too (leading lanes;
    # the parent refused this as a vec construction of the wrong width)
    'vector_narrowing': ('float4 main(float4 t : TEXCOORD0) : COLOR { float2 a = t.zyx; return float4(a, 0, 1); }\n',
                         [0.75, -0.25, 0.0, 1.0]),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='scalar-narrowing-') as tmp:
        work = Path(tmp)
        for name, (text, want) in ROWS.items():
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            src.write_text(text)
            run = subprocess.run([args.compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[-1]))
                print('  %-20s REFUSED' % name)
                continue
            got = fp_eval.evaluate(dst.read_bytes(), {'TEX0': T})
            print('  %-20s %s' % (name, 'value ok' if got == want else 'WRONG %s' % got))
            if got != want:
                failures.append('%s: got %s, want %s' % (name, got, want))
    for f in failures:
        print('FAIL:', f)
    print('scalar-narrowing: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
