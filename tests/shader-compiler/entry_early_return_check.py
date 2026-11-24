"""A fragment entry may return from inside an if (entry-early-returns).

`if (back) return a; ... if (plain) return b; ... return c;` in the entry
function was refused with "more than one return block" outside two narrow
shapes, although the same body in a helper compiled.  Such an entry that
fails as written is now compiled again with its body run the way an inlined
helper's is: each arm runs with the rest of the body and the results are
joined with selects.

Every row is judged by VALUE with the evaluator against a plain Python
rendering of the same program; inputs and constants are dyadic, so the
expected values are exact in binary32.  TEXCOORD0.xy are the operands,
TEXCOORD1.xy steer the returns.  The last row is the same program with the
body in a helper, which compiled before: it must give the same values.
"""
import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import fp_eval

HEAD = 'float4 main(float2 p : TEXCOORD0, float2 q : TEXCOORD1) : COLOR {\n'


def frac(v):
    return v - math.floor(v)


def r_two(x, y, s, t):
    base = frac(x) * 0.5
    if s < 0.5:
        return [y * 0.25, 0.0, 0.0, 1.0]
    base = base + 1.0
    if t < 0.5:
        return [base, 0.5, 0.0, 1.0]
    return [base * 2.0, y, 1.0, 1.0]


def r_nested(x, y, s, t):
    if s < 0.5:
        if t < 0.5:
            return [x, 0.0, 0.0, 1.0]
        return [y, 0.25, 0.0, 1.0]
    return [x + y, 0.5, 0.0, 1.0]


def r_else(x, y, s, t):
    v = frac(y)
    if s < 0.5:
        v = v * 2.0
    else:
        return [v, 1.0, 0.0, 1.0]
    return [v + x, 0.0, 0.0, 1.0]


def r_local(x, y, s, t):
    acc = x * 0.5
    if s < 0.5:
        acc = acc + 2.0
        if t < 0.5:
            return [acc, 0.0, 0.0, 1.0]
        acc = acc * 2.0
    return [acc + y, 0.75, 0.0, 1.0]


TWO_BODY = (' float base = frac(p.x) * 0.5;\n'
            ' if (q.x < 0.5) { return float4(p.y * 0.25, 0, 0, 1); }\n'
            ' base = base + 1.0;\n'
            ' if (q.y < 0.5) { return float4(base, 0.5, 0, 1); }\n'
            ' return float4(base * 2.0, p.y, 1, 1);\n}\n')

# name -> (source, expected(TEX0.x, TEX0.y, TEX1.x, TEX1.y) -> COLOR)
ROWS = {
    'two_returns': (HEAD + TWO_BODY, r_two),
    'nested': (HEAD + ' if (q.x < 0.5) {\n  if (q.y < 0.5) { return float4(p.x, 0, 0, 1); }\n'
               '  return float4(p.y, 0.25, 0, 1);\n }\n return float4(p.x + p.y, 0.5, 0, 1);\n}\n', r_nested),
    'else_arm': (HEAD + ' float v = frac(p.y);\n if (q.x < 0.5) { v = v * 2.0; }\n'
                 ' else { return float4(v, 1, 0, 1); }\n return float4(v + p.x, 0, 0, 1);\n}\n', r_else),
    'local_updates': (HEAD + ' float acc = p.x * 0.5;\n if (q.x < 0.5) {\n  acc = acc + 2.0;\n'
                      '  if (q.y < 0.5) { return float4(acc, 0, 0, 1); }\n  acc = acc * 2.0;\n }\n'
                      ' return float4(acc + p.y, 0.75, 0, 1);\n}\n', r_local),
    'helper_twin': ('float4 body(float2 p, float2 q) {\n' + TWO_BODY + HEAD + ' return body(p, q);\n}\n', r_two),
}
VECTORS = [(0.25, 0.5, 0.0, 0.0), (1.75, 2.25, 0.0, 1.0), (3.5, 0.75, 1.0, 0.0), (-1.25, 4.5, 1.0, 1.0),
           (2.0, 3.0, 0.25, 0.75), (5.75, -2.25, 0.75, 0.25)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='entry-early-return-') as tmp:
        for name, (source, expected) in ROWS.items():
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([args.compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                text = [l for l in (run.stdout + run.stderr).strip().splitlines() if 'error' in l or 'nv40' in l]
                failures.append('%s refused: %s' % (name, (text or ['?'])[0][:160]))
                continue
            blob = dst.read_bytes()
            for x, y, s, t in VECTORS:
                want = expected(x, y, s, t)
                try:
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0], 'TEX1': [s, t, 0.0, 1.0]}))[:4]
                except Exception as error:          # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if got != want:
                    failures.append('%s: p=(%s, %s) q=(%s, %s) got %s want %s' % (name, x, y, s, t, got, want))
                    break
    for failure in failures:
        print('FAIL:', failure)
    print('entry early return: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
