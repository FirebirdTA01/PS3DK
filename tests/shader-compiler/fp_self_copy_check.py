"""No register self-copies in fragment programs (fp-self-copies).

lerp is lowered as `MOV a; ADD delta = b - a; MAD a + t * delta`, and the
`MOV a` is there to bring `a` into the result register.  When `a` is a value
computed just before, the allocator puts it in that register already and the
MOV came out as `MOV R4.x, R4.xxxx`: every lane it writes is copied onto
itself.  Such a full-precision R-to-R copy is dropped now.

Every row is judged by VALUE with the evaluator against a plain Python
rendering of the same program, and by SHAPE: the container holds no neutral
self-copy.  Inputs and constants are dyadic, so the expected values are exact
in binary32 however the operations are ordered.  TEXCOORD0.xy supplies the
operands, TEXCOORD1.xy the two blend factors.
"""
import argparse
import math
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import fp_sources

HEAD = 'float4 main(float2 p : TEXCOORD0, float2 q : TEXCOORD1) : COLOR {\n'


def frac(v):
    return v - math.floor(v)


def lerp(a, b, t):
    return a + t * (b - a)


def r_nested(x, y, s, t):
    a, b, c = frac(x), frac(y), math.floor(x) * 0.25
    return [lerp(lerp(a, b, s), c, t), lerp(b, a, s)]


def r_vector(x, y, s, t):
    ax, ay, bx, by = frac(x), frac(y), math.floor(x) * 0.5, math.floor(y) * 0.5
    return [lerp(ax, bx, s), lerp(ay, by, s)]


def r_bilinear(x, y, s, t):
    a, b, c, d = frac(x), frac(y), frac(x + 0.5), frac(y + 0.25)
    return [lerp(lerp(a, b, s), lerp(c, d, s), t), 0.0]


def r_loop(x, y, s, t):
    total, amp = 0.0, 0.5
    for o in range(8):
        a, b, c, d = frac(x), frac(y), frac(x + 0.5), frac(y + 0.25)
        total += amp * lerp(lerp(a, b, s), lerp(c, d, s), t)
        x, y, amp = x * 2.0, y * 2.0, amp * 0.5
    return [total, 0.0]


# name -> (source, expected(TEX0.x, TEX0.y, TEX1.x, TEX1.y) -> [COLOR.x, COLOR.y])
ROWS = {
    'nested': (HEAD + ' float a = frac(p.x); float b = frac(p.y); float c = floor(p.x) * 0.25;\n'
               ' return float4(lerp(lerp(a, b, q.x), c, q.y), lerp(b, a, q.x), 0, 1);\n}\n', r_nested),
    'vector': (HEAD + ' float2 a = frac(p); float2 b = floor(p) * 0.5;\n'
               ' float2 r = lerp(a, b, q.x);\n return float4(r.x, r.y, 0, 1);\n}\n', r_vector),
    'bilinear': (HEAD + ' float a = frac(p.x); float b = frac(p.y);\n'
                 ' float c = frac(p.x + 0.5); float d = frac(p.y + 0.25);\n'
                 ' return float4(lerp(lerp(a, b, q.x), lerp(c, d, q.x), q.y), 0, 0, 1);\n}\n', r_bilinear),
    # the same blend inside a loop kept as a hardware loop
    'loop': (HEAD + ' float total = 0.0; float amp = 0.5; float2 v = p;\n'
             ' for (int o = 0; o < 8; o++) {\n'
             '  float a = frac(v.x); float b = frac(v.y);\n'
             '  float c = frac(v.x + 0.5); float d = frac(v.y + 0.25);\n'
             '  total += amp * lerp(lerp(a, b, q.x), lerp(c, d, q.x), q.y);\n'
             '  v *= 2.0; amp *= 0.5;\n }\n'
             ' return float4(total, 0, 0, 1);\n}\n', r_loop),
}
VECTORS = [(0.25, 0.5, 0.0, 0.0), (1.75, 2.25, 0.5, 0.25), (3.5, 0.75, 1.0, 0.75), (-1.25, 4.5, 0.25, 1.0),
           (2.0, 3.0, 0.75, 0.5), (5.75, -2.25, 0.5, 0.5)]


def is_self_copy(w):
    """A MOV that writes a temp from the same lanes of the same temp, nothing else."""
    if (w[2] >> 31) & 1 or (w[0] >> 24) & 0x3F != 0x01:
        return False                                    # a branch word, or not MOV
    s = fp_sources.source(w, 1)
    if s['type'] != fp_sources.TEMP or s['negate'] or s['abs'] or s['half']:
        return False
    if (w[0] >> 30) & 1 or (w[0] >> 31) & 1 or (w[0] >> 8) & 1 or (w[0] >> 7) & 1:
        return False                                    # no destination, saturate, CC write, H destination
    if (w[0] >> 22) & 3 or (w[2] >> 28) & 7 or (w[1] >> 18) & 7 != 7:
        return False                                    # precision, scale, condition
    if (w[1] >> 2) & 0x3F != (w[0] >> 1) & 0x3F:
        return False
    return all((s['swizzle'] >> (2 * k)) & 3 == k for k in range(4) if (w[0] >> (9 + k)) & 1)


def self_copies(blob):
    return [i for i, (w, _) in enumerate(fp_sources.instructions(fp_sources.ucode_words(blob))) if is_self_copy(w)]


def controls():
    """The detector on words it must and must not flag."""
    plain = (0x01000208, 0x1c9c0010, 0x0001c800, 0x0001c800)        # MOV R4.x, R4.xxxx
    assert is_self_copy(plain)
    assert not is_self_copy((plain[0], plain[1] ^ (1 << 17), plain[2], plain[3]))       # negated
    assert not is_self_copy((plain[0], 0x1c9c0008, plain[2], plain[3]))                 # from R2
    assert not is_self_copy((0x01000408, plain[1], plain[2], plain[3]))                 # R4.y from R4.x
    assert not is_self_copy((plain[0] | (1 << 22), plain[1], plain[2], plain[3]))       # half precision
    assert not is_self_copy((plain[0] | (1 << 8), plain[1], plain[2], plain[3]))        # writes CC
    assert not is_self_copy((0x03000208, plain[1], plain[2], plain[3]))                 # not a MOV


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    controls()
    failures = []
    with tempfile.TemporaryDirectory(prefix='fp-self-copy-') as tmp:
        for name, (source, expected) in ROWS.items():
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([args.compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                text = (run.stdout + run.stderr).strip().splitlines()
                failures.append('%s refused: %s' % (name, (text or ['?'])[0]))
                continue
            blob = dst.read_bytes()
            before = len(failures)
            for x, y, s, t in VECTORS:
                want = expected(x, y, s, t)
                try:
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0], 'TEX1': [s, t, 0.0, 1.0]}))[:2]
                except Exception as error:          # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if got != want:
                    failures.append('%s: p=(%s, %s) q=(%s, %s) got %s want %s' % (name, x, y, s, t, got, want))
                    break
            if len(failures) == before:
                found = self_copies(blob)
                if found:
                    failures.append('%s: self-copy at instruction %s' % (name, found))
    for failure in failures:
        print('FAIL:', failure)
    print('fp self copy: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
