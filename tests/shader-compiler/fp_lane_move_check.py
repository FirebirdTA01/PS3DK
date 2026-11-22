"""Lane-by-lane copies of one source are one MOV in fragment programs (fp-lane-moves).

A vector built from scalars was written one lane at a time, so
`float4(d, d, d, 1.0)` cost three MOVs from one register where one MOV with a
three-lane write mask does the same.  Two adjacent MOVs that write different
lanes of one register from one source, alike in everything but the write mask
and the swizzle, are merged now.  When the source is the destination register
itself the second copy may read a lane the first one wrote; such a pair stays
two instructions.

Every row is judged by VALUE with the evaluator against a plain Python
rendering of the same program (all four colour lanes), and by SHAPE: the
container holds no adjacent pair of MOVs that one MOV could carry.  Values are
compared bit for bit, so a -0.0 that became +0.0 is a failure.  Inputs and
constants are dyadic, so the expected values are exact in binary32.
TEXCOORD0.xy and TEXCOORD1.xy supply the operands.
"""
import argparse
import math
import struct
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import fp_sources

HEAD = 'float4 main(float2 p : TEXCOORD0, float2 q : TEXCOORD1) : COLOR {\n'
MASK = 0xF << 9          # write mask, word 0
SWIZZLE = 0xFF << 9      # source swizzle, word 1


def frac(v):
    return v - math.floor(v)


def r_broadcast(x, y, s, t):
    d = frac(x) * 0.5 + y
    return [d, d, d, 1.0]


def r_swizzled(x, y, s, t):
    return [frac(y), frac(x), frac(x), 1.0]


def r_interleaved(x, y, s, t):
    a, b = frac(x), math.floor(y) * 0.25
    return [a, b, a, b]


def r_shift_down(x, y, s, t):
    return [frac(y), s + 0.5, t * 0.5, t * 0.5]


def r_chain(x, y, s, t):
    return [frac(x)] * 4


# name -> (source, expected(TEX0.x, TEX0.y, TEX1.x, TEX1.y) -> COLOR)
ROWS = {
    # one scalar into three lanes, a literal into the fourth
    'broadcast': (HEAD + ' float d = frac(p.x) * 0.5 + p.y;\n return float4(d, d, d, 1.0);\n}\n', r_broadcast),
    # lanes of one vector in another order
    'swizzled': (HEAD + ' float2 a = frac(p);\n return float4(a.y, a.x, a.x, 1.0);\n}\n', r_swizzled),
    # two sources alternating: nothing adjacent shares a source
    'interleaved': (HEAD + ' float a = frac(p.x); float b = floor(p.y) * 0.25;\n'
                    ' return float4(a, b, a, b);\n}\n', r_interleaved),
    # a register copied onto itself one lane down: no copy reads a lane an
    # earlier one wrote, so the copies may share an instruction
    'shift_down': (HEAD + ' float4 c = float4(frac(p.x), frac(p.y), q.x + 0.5, q.y * 0.5);\n'
                   ' c.x = c.y; c.y = c.z; c.z = c.w;\n return c;\n}\n', r_shift_down),
    # the same one lane up: every copy reads the lane the one before it
    # wrote, so x reaches all four lanes
    # literal lanes that differ only in the sign of zero are different values:
    # the copies of +0 and of -0 may not become one copy of either
    'zero_plus_minus': (HEAD + ' return float4(0.0, -0.0, p.x, p.y);\n}\n',
                        lambda x, y, s, t: [0.0, -0.0, x, y]),
    'zero_minus_plus': (HEAD + ' return float4(-0.0, 0.0, p.x, p.y);\n}\n',
                        lambda x, y, s, t: [-0.0, 0.0, x, y]),
    'zero_assigned': (HEAD + ' float4 c = float4(p.x, p.y, 1.0, 1.0);\n c.x = 0.0; c.y = -0.0;\n'
                      ' return c;\n}\n', lambda x, y, s, t: [0.0, -0.0, 1.0, 1.0]),
    'chain': (HEAD + ' float4 c = float4(frac(p.x), frac(p.y), q.x + 0.5, q.y * 0.5);\n'
              ' c.y = c.x; c.z = c.y; c.w = c.z;\n return c;\n}\n', r_chain),
}
VECTORS = [(0.25, 0.5, 0.0, 0.0), (1.75, 2.25, 0.5, 0.25), (3.5, 0.75, 1.0, 0.75), (-1.25, 4.5, 0.25, 1.0),
           (2.0, 3.0, 0.75, 0.5), (5.75, -2.25, 0.5, 0.5)]


def bits(values):
    return [struct.pack('>f', v) for v in values]


def block(const):
    """A constant block by its bits, so +0 and -0 are different blocks."""
    if const is None:
        return None
    return tuple(struct.pack('>f', v) if isinstance(v, float) else v for v in const)


def one_mov_carries(a, ca, b, cb):
    """Two adjacent instruction words that differ only in write mask and source swizzle."""
    if (a[2] >> 31) & 1 or (b[2] >> 31) & 1:
        return False                                    # a branch word
    if (a[0] >> 24) & 0x3F != 0x01 or (b[0] >> 24) & 0x3F != 0x01:
        return False                                    # not two MOVs
    if (a[0] & ~MASK) != (b[0] & ~MASK) or (a[0] & MASK) & (b[0] & MASK):
        return False                                    # another register or stamp, or a shared lane
    if (a[1] & ~SWIZZLE) != (b[1] & ~SWIZZLE) or a[2] != b[2] or a[3] != b[3] or block(ca) != block(cb):
        return False                                    # another source or condition
    s = fp_sources.source(a, 1)
    if s['type'] == fp_sources.TEMP and not (a[0] >> 30) & 1 and (a[1] >> 2) & 0x3F == (a[0] >> 1) & 0x3F:
        return False                                    # reads its own destination register
    return True


def mergeable(blob):
    ins = list(fp_sources.instructions(fp_sources.ucode_words(blob)))
    return [i for i in range(len(ins) - 1) if one_mov_carries(ins[i][0], ins[i][1], ins[i + 1][0], ins[i + 1][1])]


def controls():
    """The detector on pairs it must and must not flag."""
    x = (0x01000208, 0x1c9c0008, 0x0001c800, 0x0001c800)            # MOV R4.x, R2.xxxx
    y = (0x01000408, 0x1c9c0008, 0x0001c800, 0x0001c800)            # MOV R4.y, R2.xxxx
    assert one_mov_carries(x, None, y, None)
    assert one_mov_carries(x, None, (y[0], y[1] ^ (1 << 9), y[2], y[3]), None)      # another swizzle
    assert not one_mov_carries(x, None, x, None)                                    # the same lane
    assert not one_mov_carries(x, None, (0x0100040a, y[1], y[2], y[3]), None)       # R5.y
    assert not one_mov_carries(x, None, (y[0], 0x1c9c000c, y[2], y[3]), None)       # from R3
    assert not one_mov_carries(x, None, (y[0], y[1] ^ (1 << 17), y[2], y[3]), None)  # negated
    assert not one_mov_carries(x, None, (y[0] | (1 << 22), y[1], y[2], y[3]), None)  # half precision
    assert not one_mov_carries(x, None, (0x03000408, y[1], y[2], y[3]), None)       # not a MOV
    own = 0x1c9c0010                                                                # from R4 itself
    assert not one_mov_carries((x[0], own, x[2], x[3]), None, (y[0], own, y[2], y[3]), None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    controls()
    failures = []
    with tempfile.TemporaryDirectory(prefix='fp-lane-move-') as tmp:
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
                    got = list(fp_eval.evaluate(blob, {'TEX0': [x, y, 0.0, 1.0], 'TEX1': [s, t, 0.0, 1.0]}))[:4]
                except Exception as error:          # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if bits(got) != bits(want):          # by bits: -0.0 == 0.0 as floats
                    failures.append('%s: p=(%s, %s) q=(%s, %s) got %s want %s' % (name, x, y, s, t, got, want))
                    break
            if len(failures) == before:
                found = mergeable(blob)
                if found:
                    failures.append('%s: MOV pair one MOV could carry at instruction %s' % (name, found))
    for failure in failures:
        print('FAIL:', failure)
    print('fp lane moves: tests=%d pass=%d fail=%d; %d vectors per row'
          % (len(ROWS), len(ROWS) - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
