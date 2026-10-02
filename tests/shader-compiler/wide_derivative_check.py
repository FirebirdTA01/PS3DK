"""A float3/float4 ddx/ddy is two two-lane DDX/DDY (community blur family).

NV40's DDX and DDY produce two lanes.  Measured on sce-cgc 475:

    float4 ddx(v):  DDXR R1.xy, R0.zwzw;  MOVR R1.zw, R1.xyxy;  DDXR R1.xy, R0;
    float3 ddy(v):  DDYR R1.x,  R0.zwzw;  MOVR R1.z,  R1.xyxy;  DDYR R1.xy, R0;

i.e. the high lanes are differentiated first into a temp and moved up, then
the low lanes are written in place.  The evaluators have no derivative model
(a derivative needs the neighbouring pixel), so this checks the shape lane
by lane: the high derivative reads the SOURCE's z and w, the move carries
exactly those two results to z and w of the register the low derivative
then completes from the source's x and y.  A defect that differentiated x
twice, or moved the wrong lanes, fails here.
"""
import sys

from fp_sources import instructions, source, ucode_words

DDX, DDY, MOV = 0x15, 0x16, 0x01


def lanes(swz):
    return [(swz >> (2 * i)) & 3 for i in range(4)]


def plain(w):
    """None, or the modifier this shape check does not allow (review: codex)."""
    if (w[1] >> 18) & 7 != 7:
        return 'a predicated instruction (condition %d)' % ((w[1] >> 18) & 7)
    if (w[0] >> 7) & 1:
        return 'a half-bank destination'
    if (w[0] >> 31) & 1:
        return 'saturation'
    if (w[2] >> 28) & 7:
        return 'an output scale'
    src = source(w, 1)
    if src['negate'] or src['abs']:
        return 'a negated or absolute source'
    return None


def check(blob, op, width):
    rows = [w for w, _ in instructions(ucode_words(blob))]
    ops = [(w[0] >> 24) & 63 for w in rows]
    d = [i for i, o in enumerate(ops) if o == op]
    if len(d) != 2:
        return '%d derivative instructions, want 2' % len(d)
    hi, lo = rows[d[0]], rows[d[1]]
    dst = lambda w: (w[0] >> 1) & 63
    mask = lambda w: (w[0] >> 9) & 15
    for which, w in (('high derivative', hi), ('low derivative', lo)):
        why = plain(w)
        if why:
            return '%s has %s' % (which, why)
    hs, ls = source(hi, 1), source(lo, 1)
    if (hs['type'], hs['name']) != (ls['type'], ls['name']):
        return 'the two derivatives read different operands (%s, %s)' % (hs['name'], ls['name'])
    want_hi = 0x3 if width == 4 else 0x1
    if mask(hi) != want_hi or mask(lo) != 0x3:
        return 'masks hi %#x lo %#x, want %#x and 0x3' % (mask(hi), mask(lo), want_hi)
    src_lanes = lanes(ls['swizzle'])
    if lanes(hs['swizzle'])[:2] != [src_lanes[2], src_lanes[3]] and width == 4:
        return 'high derivative reads lanes %s, want the source z,w %s' % (lanes(hs['swizzle'])[:2], src_lanes[2:])
    if width == 3 and lanes(hs['swizzle'])[0] != src_lanes[2]:
        return 'high derivative reads lane %d, want the source z %d' % (lanes(hs['swizzle'])[0], src_lanes[2])
    movs = [rows[i] for i in range(d[0] + 1, d[1]) if ops[i] == MOV and source(rows[i], 1)['name'] == 'R%d' % dst(hi)]
    if len(movs) != 1:
        return 'no single move of the high result between the derivatives'
    mv = movs[0]
    if plain(mv):
        return 'the move has %s' % plain(mv)
    want_mv = 0xC if width == 4 else 0x4
    if mask(mv) != want_mv or dst(mv) != dst(lo):
        return 'move writes R%d mask %#x, want R%d mask %#x' % (dst(mv), mask(mv), dst(lo), want_mv)
    ml = lanes(source(mv, 1)['swizzle'])
    if ml[2] != 0 or (width == 4 and ml[3] != 1):
        return 'move carries lanes %s into z,w; want x,y' % ml[2:]
    return None


def self_test():
    """The checker must accept the reference shape and reject defect shapes."""
    from fp_eval import _container, _ins, _src
    from fp_sources import INPUT, TEMP
    tex0 = 0x4
    zwzw, xyxy, xyzw = 0xEE, 0x44, 0xE4

    def prog(hi_swz=zwzw, mv_swz=xyxy, mv_dst=1, hi_neg=0):
        return _container(_ins(DDY, 2, 0x3, [_src(INPUT, swz=hi_swz, neg=hi_neg)], sel=tex0)
                          + _ins(MOV, mv_dst, 0xC, [_src(TEMP, 2, swz=mv_swz)])
                          + _ins(DDY, 1, 0x3, [_src(INPUT, swz=xyzw)], sel=tex0)
                          + _ins(MOV, 0, 0xF, [_src(TEMP, 1)], end=1))
    rows = [('green: reference shape', prog(), True),
            ('red: high derivative reads x,y', prog(hi_swz=xyzw), False),
            ('red: move carries z,w', prog(mv_swz=0xEE), False),
            ('red: move lands in another register', prog(mv_dst=3), False),
            ('red: high half only negated', prog(hi_neg=1), False)]
    ok = True
    for name, blob, good in rows:
        why = check(blob, DDY, 4)
        hit = (why is None) == good
        ok &= hit
        print('  %-40s %s  (%s)' % (name, 'ok' if hit else 'FAIL', why or 'accepted'))
    return ok


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(0 if self_test() else 1)
    blob = open(sys.argv[1], 'rb').read()
    why = check(blob, DDX if sys.argv[2] == 'ddx' else DDY, int(sys.argv[3]))
    print(why or 'ok')
    sys.exit(1 if why else 0)
