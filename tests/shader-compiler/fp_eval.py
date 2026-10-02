"""Evaluate a straight-line NV40 fragment program numerically (binary32).

The existing value checkers are fixture-specific.  This one runs any program
built only from the ops it models, on caller-supplied inputs, and returns the
colour output, so two containers (ours and the reference's) can be compared
by VALUE when their bytes differ.  It refuses, with a reason, anything it does
not model rather than stepping over it:

  - reading a register lane that was never written, or a colour output whose
    four lanes were not all written (no lane defaults to zero);
  - predicated instructions (condition test other than TR), branches, KIL;
  - reading an R lane after either of its H halves was written, or an H lane
    after its R register was written (H2k/H2k+1 alias Rk with an unmodelled
    lane layout, so every lane of the other bank goes stale until rewritten);
  - an output scale other than 1x;
  - an fp16 or fx12 instruction whose source values are not exactly
    representable at that precision (fx12: multiples of 1/1024 in [-2, 2)).
    For representable sources the low-precision result equals the binary32
    one for the ops modelled here, so callers pick inputs in that domain and
    anything outside it stays unjudged rather than guessed;
  - any opcode outside MODELLED.

Saturation, negate, absolute value, swizzles, write masks, inline constant
blocks, the instruction input selector and half-precision registers (values
rounded to binary16 on write) are modelled.  The colour output is R0, or H0
when the container's outputFromH0 byte (programOffset + 19) is set.
Arithmetic is rounded to binary32 after every operation.
"""
import math
import struct

from fp_sources import CONST, INPUT, TEMP, ARITY, instructions, source, ucode_words

MOV, MUL, ADD, MAD, DP3, DP4, MIN, MAX = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09
SLT, SGE, SLE, SGT, SNE, SEQ = 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
FRC, FLR, FENCBR, DP2 = 0x10, 0x11, 0x3E, 0x38
MODELLED = {MOV, MUL, ADD, MAD, DP3, DP4, MIN, MAX, SLT, SGE, SLE, SGT, SNE, SEQ, FRC, FLR, FENCBR, DP2}

INPUT_SEL = {0x1: "COL0", 0x2: "COL1", 0x4: "TEX0", 0x5: "TEX1", 0x6: "TEX2", 0x7: "TEX3",
             0x8: "TEX4", 0x9: "TEX5", 0xA: "TEX6", 0xB: "TEX7"}


class Unmodelled(Exception):
    pass


def f32(x):
    return struct.unpack('>f', struct.pack('>f', x))[0]


def _const_floats(block):
    # ucode_words() already returns LOGICAL (unswapped) words, constants too.
    return [struct.unpack('>f', struct.pack('>I', w))[0] for w in block]


def f16(x):
    return struct.unpack('<e', struct.pack('<e', x))[0]


class _Regs:
    """Per-lane register file.  A lane is readable only once written and while
    not stale; H2k/H2k+1 alias Rk with an unmodelled bit layout, so a write to
    either bank marks EVERY lane of the other bank stale until rewritten."""

    def __init__(self):
        self.vals, self.defined, self.stale = {}, {}, {}

    def read(self, key, lanes):
        for lane in lanes:
            if lane not in self.defined.get(key, ()):
                raise Unmodelled("%s%d.%s read before it was written" % (key + ("xyzw"[lane],)))
            if lane in self.stale.get(key, ()):
                raise Unmodelled("%s%d.%s read after its aliased bank was written" % (key + ("xyzw"[lane],)))
        return self.vals.get(key, [0.0] * 4)

    def write(self, key, mask, res):
        cur = self.vals.get(key, [0.0] * 4)
        lanes = [i for i in range(4) if mask & (1 << i)]
        self.vals[key] = [res[i] if i in lanes else cur[i] for i in range(4)]
        self.defined.setdefault(key, set()).update(lanes)
        self.stale.setdefault(key, set()).difference_update(lanes)
        if not lanes:
            return
        bank, reg = key
        others = [('R', reg // 2)] if bank == 'H' else [('H', 2 * reg), ('H', 2 * reg + 1)]
        for other in others:
            if self.defined.get(other):
                self.stale.setdefault(other, set()).update(range(4))


def _read(w, slot, regs, inputs, const, lanes):
    s = source(w, slot)
    swz = s["swizzle"]
    comps = [(swz >> (2 * i)) & 3 for i in range(4)]
    if s["type"] == TEMP:
        key = ('H' if s["half"] else 'R', s["reg"])
        v = regs.read(key, sorted({comps[i] for i in lanes}))
    elif s["type"] == INPUT:
        sel = (w[0] >> 13) & 0xF
        name = INPUT_SEL.get(sel)
        if name is None or name not in inputs:
            raise Unmodelled("input selector %#x not supplied" % sel)
        v = inputs[name]
    elif s["type"] == CONST:
        if const is None:
            raise Unmodelled("const source without a block")
        v = _const_floats(const)
    else:
        raise Unmodelled("source type %d" % s["type"])
    out = [v[comps[i]] if i in lanes else 0.0 for i in range(4)]
    if s["abs"]:
        out = [abs(x) for x in out]
    if s["negate"]:
        out = [-x for x in out]
    return out


def _op(opc, a, b, c):
    cmp = {SLT: lambda x, y: x < y, SGE: lambda x, y: x >= y, SLE: lambda x, y: x <= y,
           SGT: lambda x, y: x > y, SNE: lambda x, y: x != y, SEQ: lambda x, y: x == y}
    if opc == MOV:
        return a
    if opc == MUL:
        return [f32(x * y) for x, y in zip(a, b)]
    if opc == ADD:
        return [f32(x + y) for x, y in zip(a, b)]
    if opc == MAD:
        return [f32(f32(x * y) + z) for x, y, z in zip(a, b, c)]
    if opc in (DP2, DP3, DP4):
        n = {DP2: 2, DP3: 3, DP4: 4}[opc]
        acc = 0.0
        for i in range(n):
            acc = f32(acc + f32(a[i] * b[i]))
        return [acc] * 4
    if opc == MIN:
        return [min(x, y) for x, y in zip(a, b)]
    if opc == MAX:
        return [max(x, y) for x, y in zip(a, b)]
    if opc in cmp:
        return [1.0 if cmp[opc](x, y) else 0.0 for x, y in zip(a, b)]
    if opc == FRC:
        return [f32(x - math.floor(x)) for x in a]
    if opc == FLR:
        return [float(math.floor(x)) for x in a]
    raise Unmodelled("opcode %#x" % opc)


def evaluate(blob, inputs):
    """Run the program; `inputs` maps 'TEX0'.. / 'COL0' to 4-float lists. Returns R0."""
    regs = _Regs()
    ended = False
    for w, const in instructions(ucode_words(blob)):
        opc = (w[0] >> 24) & 0x3F
        if (w[2] >> 31) & 1:
            raise Unmodelled("branch instruction")
        if opc not in MODELLED:
            raise Unmodelled("opcode %#x" % opc)
        if opc != FENCBR:
            if (w[1] >> 18) & 7 != 7:
                raise Unmodelled("predicated instruction (cond %d)" % ((w[1] >> 18) & 7))
            prec = (w[0] >> 22) & 3
            if prec == 3:
                raise Unmodelled("precision 3")
            if (w[2] >> 28) & 3:
                raise Unmodelled("output scale %d" % ((w[2] >> 28) & 3))
            n = ARITY.get(opc, 0)
            out_none = (w[0] >> 30) & 1
            mask = (w[0] >> 9) & 0xF
            if out_none:
                # its only effect would be a condition code, and every
                # predicated consumer is refused above
                lanes = []
            elif opc in (DP2, DP3, DP4):
                lanes = list(range({DP2: 2, DP3: 3, DP4: 4}[opc]))
            else:
                lanes = [i for i in range(4) if mask & (1 << i)]
            srcs = [_read(w, slot, regs, inputs, const, lanes) for slot in range(1, n + 1)]
            for v in srcs:
                for x in (v[i] for i in lanes):
                    if prec == 1 and f16(x) != x:
                        raise Unmodelled("fp16 instruction on a non-fp16 value %r" % x)
                    if prec == 2 and not (-2.0 <= x < 2.0 and x * 1024 == int(x * 1024)):
                        raise Unmodelled("fx12 instruction on a non-fx12 value %r" % x)
            while len(srcs) < 3:
                srcs.append([0.0] * 4)
            res = _op(opc, *srcs)
            if (w[0] >> 31) & 1:
                res = [min(1.0, max(0.0, x)) for x in res]
            if not out_none:
                half = (w[0] >> 7) & 1
                if half:
                    res = [f16(x) for x in res]
                regs.write(('H' if half else 'R', (w[0] >> 1) & 0x3F), mask, res)
        if w[0] & 1:
            ended = True
            break
    if not ended:
        raise Unmodelled("no PROGRAM_END")
    prog = struct.unpack_from(">8I", blob, 0)[5]
    key = ('H', 0) if blob[prog + 19] else ('R', 0)
    return list(regs.read(key, [0, 1, 2, 3]))


# ---- self-test: encoded controls, no compiler needed -----------------------
def _src(kind, reg=0, swz=0xE4, half=0, neg=0):
    return kind | (reg << 2) | (half << 8) | (swz << 9) | (neg << 17)


def _ins(opc, dst, mask, srcs, half=0, end=0, out_none=0, sel=0):
    from fp_sources import TEMP as T
    s = list(srcs) + [_src(T)] * (3 - len(srcs))
    w0 = end | (dst << 1) | (half << 7) | (mask << 9) | (sel << 13) | (opc << 24) | (out_none << 30)
    return [w0, s[0] | (7 << 18), s[1], s[2]]


def _container(words, h0=0):
    from fp_sources import FP_PROFILE, FORMAT_REVISION
    swap = lambda v: ((v >> 16) | (v << 16)) & 0xffffffff
    ucode = b''.join(struct.pack('>I', swap(w)) for w in words)
    total = 64 + len(ucode)
    head = struct.pack('>8I', FP_PROFILE, FORMAT_REVISION, total, 0, 0, 32, len(ucode), 64)
    sub = bytearray(22)
    sub[19] = h0
    return head + bytes(sub) + bytes(64 - 32 - 22) + ucode


def self_test():
    T, I = TEMP, INPUT
    tex0 = 0x4
    a = [0.25, -0.5, 1.0, 1.5]
    rows = []
    # GREEN controls
    rows.append(('green: MOV R0, TEX0', _container(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0, end=1)), a))
    rows.append(('green: R1=TEX0; R0=R1+R1',
                 _container(_ins(MOV, 1, 0xF, [_src(I)], sel=tex0) + _ins(ADD, 0, 0xF, [_src(T, 1), _src(T, 1)], end=1)),
                 [2 * x for x in a]))
    rows.append(('green: H0 output (outputFromH0)', _container(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0, half=1, end=1), h0=1), a))
    # RED controls (codex review of fae9ca52): each must be refused, not evaluated
    rows.append(('red: MOV R0, R5 (undefined source)', _container(_ins(MOV, 0, 0xF, [_src(T, 5)], end=1)), None))
    rows.append(('red: R0=.., H0.x=.., R0.w=.. (stale xyz output)',
                 _container(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0) + _ins(MOV, 0, 0x1, [_src(I)], sel=tex0, half=1)
                            + _ins(MOV, 0, 0x8, [_src(I)], sel=tex0, end=1)), None))
    rows.append(('red: OUT_NONE only (output never written)',
                 _container(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0, out_none=1, end=1)), None))
    rows.append(('red: R0.xy written, output needs zw',
                 _container(_ins(MOV, 0, 0x3, [_src(I)], sel=tex0, end=1)), None))
    fails = 0
    for name, blob, want in rows:
        try:
            got = evaluate(blob, {'TEX0': a})
            ok = want is not None and got == want
            detail = 'got %s' % got
        except Unmodelled as e:
            ok = want is None
            detail = 'refused: %s' % e
        fails += not ok
        print('  %-50s %s  (%s)' % (name, 'ok' if ok else 'FAIL', detail))
    print('fp_eval self-test: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return fails == 0


if __name__ == '__main__':
    import sys
    sys.exit(0 if self_test() else 1)
