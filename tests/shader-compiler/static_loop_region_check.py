"""Large fixed-count fragment loops kept as hardware loops (static-loop-regions).

A `for` whose trip sequence is proven used to be expanded always, so a noise
octave loop cost its whole body once per trip.  The established compiler
expands such a loop only while trips * (body + 1) is at most 150 operations
and emits LOOP {trips, 0, 1} past that; so do we now, including a loop that
opens with one `if (c) break;`.  A loop the hardware path does not take is
compiled again expanded, so keeping a loop never costs acceptance.

Every row is judged by VALUE with the evaluator against a plain Python
rendering of the same program, and by SHAPE: the trip count of every LOOP in
the container.  All arithmetic stays exact in binary32 (dyadic inputs and
constants, no division), so the expected values do not depend on how the
operations are ordered or fused.  TEXCOORD0.xy is the sample point,
TEXCOORD1.x the run-time limit of the breaking loop.
"""
import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import fp_eval

NOISE = '''float h(float2 p) { return frac(dot(p, float2(0.5, 0.25)) + p.x * 0.125); }
float vn(float2 p) {
    float2 i = floor(p); float2 f = frac(p); float2 u = f * f * (3.0 - 2.0 * f);
    float a = h(i); float b = h(i + float2(1.0, 0.0));
    float c = h(i + float2(0.0, 1.0)); float d = h(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
'''
FBM4 = ('float fbm(float2 p) {\n float sum = 0.0; float amp = 0.5;\n'
        ' for (int o = 0; o < 4; o++) { sum += amp * vn(p); p *= 2.0; amp *= 0.5; }\n'
        ' return sum;\n}\n')
FBM6 = ('float fbmn(float2 p, float n) {\n float sum = 0.0; float amp = 0.5;\n'
        ' for (int o = 0; o < 6; o++) { if (float(o) >= n) { break; } sum += amp * vn(p); p *= 2.0; amp *= 0.5; }\n'
        ' return sum;\n}\n')
HEAD = 'float4 main(float2 uv : TEXCOORD0, float2 q : TEXCOORD1) : COLOR {\n'


def h(x, y):
    v = 0.5 * x + 0.25 * y + 0.125 * x
    return v - math.floor(v)


def vn(x, y):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    ux, uy = fx * fx * (3.0 - 2.0 * fx), fy * fy * (3.0 - 2.0 * fy)
    a, b, c, d = h(ix, iy), h(ix + 1, iy), h(ix, iy + 1), h(ix + 1, iy + 1)
    l1, l2 = a + ux * (b - a), c + ux * (d - c)
    return l1 + uy * (l2 - l1)


def fbm(x, y, trips=4, limit=None, weight=lambda o: 1.0):
    s, amp = 0.0, 0.5
    for o in range(trips):
        if limit is not None and o >= limit:
            break
        s += weight(o) * amp * vn(x, y)
        x, y, amp = x * 2.0, y * 2.0, amp * 0.5
    return s


def r_small(x, y, n):
    s = 0.0
    for o in range(4):
        s += x * 0.5
    return [s, 0.0]


def r_array(x, y, n):
    acc = []
    for o in range(4):
        acc.append(vn(x, y))
        x, y = x * 2.0, y * 2.0
    return [acc[0] + acc[1], acc[2] + acc[3]]


# name -> (source, expected(TEX0.x, TEX0.y, TEX1.x) -> [COLOR.x, COLOR.y], LOOP trip counts)
ROWS = {
    # the loop in a helper, in the entry, and expanded from two calls
    'helper': (NOISE + FBM4 + HEAD + ' return float4(fbm(uv), 0, 0, 1);\n}\n',
               lambda x, y, n: [fbm(x, y), 0.0], [4]),
    'entry': (NOISE + HEAD + ' float sum = 0.0; float amp = 0.5; float2 p = uv;\n'
              ' for (int o = 0; o < 4; o++) { sum += amp * vn(p); p *= 2.0; amp *= 0.5; }\n'
              ' return float4(sum, amp, 0, 1);\n}\n',
              lambda x, y, n: [fbm(x, y), 0.03125], [4]),
    'two_calls': (NOISE + FBM4 + HEAD + ' return float4(fbm(uv), fbm(uv + float2(2.0, 1.0)), 0, 1);\n}\n',
                  lambda x, y, n: [fbm(x, y), fbm(x + 2.0, y + 1.0)], [4, 4]),
    # a leading break is the BRK at the head of the iteration
    'head_break': (NOISE + FBM6 + HEAD + ' return float4(fbmn(uv, q.x), 0, 0, 1);\n}\n',
                   lambda x, y, n: [fbm(x, y, 6, n), 0.0], [6]),
    # the body reads the induction: it is carried and stepped
    'induction_read': (NOISE + HEAD + ' float sum = 0.0; float amp = 0.5; float2 p = uv;\n'
                       ' for (int o = 0; o < 4; o++) { sum += float(o) * amp * vn(p); p *= 2.0; amp *= 0.5; }\n'
                       ' return float4(sum, 0, 0, 1);\n}\n',
                       lambda x, y, n: [fbm(x, y, weight=float), 0.0], [4]),
    # a small loop is expanded as before
    'small_expanded': (HEAD + ' float s = 0.0;\n for (int o = 0; o < 4; o++) { s += uv.x * 0.5; }\n'
                       ' return float4(s, 0, 0, 1);\n}\n', r_small, []),
    # a large loop the hardware path does not take (it writes an array
    # element) is compiled again expanded
    'array_write_expanded': (NOISE + HEAD + ' float acc[4]; float2 p = uv;\n'
                             ' for (int o = 0; o < 4; o++) { acc[o] = vn(p); p *= 2.0; }\n'
                             ' return float4(acc[0] + acc[1], acc[2] + acc[3], 0, 1);\n}\n', r_array, []),
}
VECTORS = [(0.25, 0.5, 0.0), (1.75, 2.25, 1.0), (3.5, 0.75, 2.5), (-1.25, 4.5, 6.0), (2.0, 3.0, 9.0),
           (5.75, -2.25, -1.0)]


def loop_counts(blob):
    counts = []
    for w, _ in fp_eval.instructions(fp_eval.ucode_words(blob)):
        if (w[2] >> 31) & 1 and (w[0] >> 24) & 0x3F == 0x03:
            counts.append((w[2] >> 2) & 0xFF)
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix='static-loop-region-') as tmp:
        for name, (source, expected, loops) in ROWS.items():
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([args.compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                text = (run.stdout + run.stderr).strip().splitlines()
                lines = [l for l in text if 'error' in l or 'refus' in l]
                failures.append('%s refused: %s' % (name, (lines or text or ['?'])[0]))
                continue
            blob = dst.read_bytes()
            got_loops = loop_counts(blob)
            if got_loops != loops:
                failures.append('%s: LOOP trip counts %s, want %s' % (name, got_loops, loops))
                continue
            for x, y, n in VECTORS:
                want = expected(x, y, n)
                try:
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0], 'TEX1': [n, 0.0, 0.0, 1.0]}))[:2]
                except Exception as error:      # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if got != want:
                    failures.append('%s: uv=(%s, %s) n=%s got %s want %s' % (name, x, y, n, got, want))
                    break
    for failure in failures:
        print('FAIL:', failure)
    print('static loop regions: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
