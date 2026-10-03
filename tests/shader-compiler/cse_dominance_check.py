"""CSE reuses a value only where its defining block dominates the use.

libretro's ddt family assigns `C = A + D - B` in one arm of a nested if/else
and `B = A + D - C` in the other; both arms compute A + D, and the pass
replaced the else arm's copy with the then arm's value - not defined on the
else path - so the program was refused (unresolved operand, 10 community
programs).  Rows are judged by value on inputs that drive every path
(inner then, inner else, outer else-if, neither); each expected value is the
C formula.  The reference accepts this program, but its container aliases H
and R registers, which fp_eval refuses to model, so the formula is the
judge here.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

SRC = """float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR
{
    float3 A = t0.xyz, B = t1.xyz, C = t0.zyx, D = t1.zyx;
    if (t0.w < t1.w)
    {
        if (t0.x < t1.x) C = A + D - B;
        else B = A + D - C;
    }
    else if (t0.w > t1.w)
    {
        D = B + C - A;
    }
    return float4(A + B + C + D, 1.0);
}
"""


def ref(t0, t1):
    A, B, C, D = t0[:3], t1[:3], [t0[2], t0[1], t0[0]], [t1[2], t1[1], t1[0]]
    add = lambda u, v: [a + b for a, b in zip(u, v)]
    sub = lambda u, v: [a - b for a, b in zip(u, v)]
    if t0[3] < t1[3]:
        if t0[0] < t1[0]:
            C = sub(add(A, D), B)
        else:
            B = sub(add(A, D), C)
    elif t0[3] > t1[3]:
        D = sub(add(B, C), A)
    s = add(add(add(A, B), C), D)
    return s + [1.0]


INPUTS = [  # (t0, t1): inner then, inner else, outer else-if, neither
    ([0.25, 0.5, 1.0, 0.0], [0.75, 0.125, 0.5, 1.0]),
    ([1.0, 0.5, 0.25, 0.0], [0.5, 0.75, 0.125, 1.0]),
    ([0.25, 0.5, 1.0, 2.0], [0.75, 0.125, 0.5, 1.0]),
    ([0.25, 0.5, 1.0, 1.0], [0.75, 0.125, 0.5, 1.0]),
]


# Both arms of an if/else compute the same expression (one return block).
# The fragment general path flattens the program in reverse post-order, so a
# value from the arm EARLIER in that order may be shared with the later arm
# (xbrz-freescale-pass1 needs that sharing to stay within the temp budget).
# Judged by value on both paths.
SHARED = """float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : COLOR
{
    float3 r;
    if (t0.w < t1.w) r = (t0.xyz + t1.xyz) * 2.0;
    else r = (t0.xyz + t1.xyz) - t1.zyx;
    return float4(r, 1.0);
}
"""


def shared_ref(t0, t1):
    s = [a + b for a, b in zip(t0[:3], t1[:3])]
    if t0[3] < t1[3]:
        return [x * 2.0 for x in s] + [1.0]
    return [s[0] - t1[2], s[1] - t1[1], s[2] - t1[0], 1.0]


def main():
    compiler = sys.argv[1]
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='cse-dominance-') as tmp:
        src, dst = Path(tmp) / 'nested.cg', Path(tmp) / 'nested.bin'
        src.write_text(SRC)
        run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                             capture_output=True, text=True, timeout=60)
        if run.returncode != 0 or not dst.exists():
            failures.append('nested if/else refused: %s' % (run.stderr.strip().splitlines() or ['?'])[-1])
        else:
            blob = dst.read_bytes()
            for t0, t1 in INPUTS:
                got, want = fp_eval.evaluate(blob, {'TEX0': t0, 'TEX1': t1}), ref(t0, t1)
                ok = got == want
                print('  %-34s %s' % ('%s / %s' % (t0, t1), 'value ok' if ok else 'WRONG %s want %s' % (got, want)))
                if not ok:
                    failures.append('nested if/else: got %s for %s/%s, want %s' % (got, t0, t1, want))
        src, dst = Path(tmp) / 'shared.cg', Path(tmp) / 'shared.bin'
        src.write_text(SHARED)
        run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                             capture_output=True, text=True, timeout=60)
        if run.returncode != 0 or not dst.exists():
            failures.append('shared-arm expression refused: %s' % (run.stderr.strip().splitlines() or ['?'])[-1])
        else:
            blob = dst.read_bytes()
            for t0, t1 in INPUTS:
                got, want = fp_eval.evaluate(blob, {'TEX0': t0, 'TEX1': t1}), shared_ref(t0, t1)
                print('  %-34s %s' % ('shared %s / %s' % (t0[3], t1[3]), 'value ok' if got == want else 'WRONG %s want %s' % (got, want)))
                if got != want:
                    failures.append('shared-arm expression: got %s for %s/%s, want %s' % (got, t0, t1, want))
    for f in failures:
        print('FAIL:', f)
    print('cse-dominance: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
