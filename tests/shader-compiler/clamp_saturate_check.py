"""clamp(x, 0, 1) of a COMPUTED value saturates; of a plain input it is min/max.

The pixel judge found crt-ddt painting full white where the reference paints
black: its GAMMA_OUT pow() gives a NaN lane, and clamp(NaN, 0, 1) as MIN with
1 then MAX with 0 is 1 on the GPU, while the reference folds the clamp into
the producing instruction as a saturate (EX2R_sat; DIVR_SAT below), which is
0.  For a plain input the reference keeps MIN then MAX (MINR, MAXR for
clamp(t.z, 0, 1)), and so do we.  Every expected value is measured on the
reference compiler's output for this source:
  t = (0, 0, .5, .5)       -> (0, .5, .5, 1)   0/0 = NaN saturates to 0
  t = (1, 0, 1.5, .1)      -> (1, 1, .25, 1)   +Inf -> 1; input 1.5 -> 1
  t = (-1, 0, -.5, .9)     -> (0, 0, .75, 1)   -Inf -> 0; input -.5 -> 0
  t = (.25, .5, .75, .5)   -> (.5, .75, .5, 1) finite, unclamped
The third lane is a clamp with bounds .25/.75 (never a saturate).
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

SRC = """float4 main(float4 t : TEXCOORD0) : COLOR
{
    float q = t.x / t.y;
    return float4(clamp(q, 0.0, 1.0), clamp(t.z, 0.0, 1.0), clamp(t.w, 0.25, 0.75), 1.0);
}
"""

ROWS = [
    ([0.0, 0.0, 0.5, 0.5], [0.0, 0.5, 0.5, 1.0]),
    ([1.0, 0.0, 1.5, 0.1], [1.0, 1.0, 0.25, 1.0]),
    ([-1.0, 0.0, -0.5, 0.9], [0.0, 0.0, 0.75, 1.0]),
    ([0.25, 0.5, 0.75, 0.5], [0.5, 0.75, 0.5, 1.0]),
]


def main():
    compiler = sys.argv[1]
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='clamp-saturate-') as tmp:
        src, dst = Path(tmp) / 'sat.cg', Path(tmp) / 'sat.bin'
        src.write_text(SRC)
        run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                             capture_output=True, text=True, timeout=60)
        if run.returncode != 0 or not dst.exists():
            failures.append('refused: %s' % (run.stderr.strip().splitlines() or ['?'])[0])
        else:
            blob = dst.read_bytes()
            for t, want in ROWS:
                got = fp_eval.evaluate(blob, {'TEX0': t})
                ok = got == want
                print('  %-26s %s' % (t, 'value ok' if ok else 'WRONG %s want %s' % (got, want)))
                if not ok:
                    failures.append('%s: got %s, want %s' % (t, got, want))
    for f in failures:
        print('FAIL:', f)
    print('clamp-saturate: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
