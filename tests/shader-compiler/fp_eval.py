"""Evaluate a straight-line NV40 fragment program numerically (binary32).

The existing value checkers are fixture-specific.  This one runs any program
built only from the ops it models, on caller-supplied inputs, and returns the
colour output, so two containers (ours and the reference's) can be compared
by VALUE when their bytes differ.  It refuses, with a reason, anything it does
not model rather than stepping over it:

  - reading a register lane that was never written, or a colour output whose
    four lanes were not all written (no lane defaults to zero);
  - a predicated instruction whose condition-code lane was never written, or
    a condition-code write of a NaN; branches and KIL;
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

The condition-code register is modelled per lane: an instruction with the
condition-write bit (word 0 bit 8) stores its result lanes (after saturation
and any fp16 rounding) under its write mask, also when OUT_NONE; a condition
test (word 1 bits 18-20, swizzle bits 21-28) gates each destination lane on
the CC lane its swizzle selects.  RC and HC writes share this one register in
the encoding (no register-select bit differs between them).
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
FENCTR = 0x3D   # texture fence: like FENCBR, no value effect
RCP = 0x1A   # nvfx_shader.h NVFX_FP_OP_OPCODE_RCP: scalar, reads the source's x lane
DIV = 0x3A   # NVFX_FP_OP_OPCODE_DIV (NV_fragment_program2 DIV): vector src0 / scalar src1.x
MODELLED = {MOV, MUL, ADD, MAD, DP3, DP4, MIN, MAX, SLT, SGE, SLE, SGT, SNE, SEQ, FRC, FLR, FENCBR, FENCTR, DP2, RCP, DIV}

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
    if opc == DIV:
        y = b[0]
        return [f32(x / y) if y != 0 else (math.nan if x == 0 else math.copysign(math.inf, x) * math.copysign(1, y))
                for x in a]
    if opc == RCP:
        x = a[0]
        return [f32(1.0 / x) if x != 0 else math.copysign(math.inf, x)] * 4
    raise Unmodelled("opcode %#x" % opc)


_CC_TEST = {0: lambda x: False, 1: lambda x: x < 0, 2: lambda x: x == 0, 3: lambda x: x <= 0,
            4: lambda x: x > 0, 5: lambda x: x != 0, 6: lambda x: x >= 0, 7: lambda x: True}


def evaluate(blob, inputs):
    """Run the program; `inputs` maps 'TEX0'.. / 'COL0' to 4-float lists. Returns R0."""
    regs = _Regs()
    cc = [None] * 4
    ended = False
    for w, const in instructions(ucode_words(blob)):
        opc = (w[0] >> 24) & 0x3F
        if (w[2] >> 31) & 1:
            raise Unmodelled("branch instruction")
        if opc not in MODELLED:
            raise Unmodelled("opcode %#x" % opc)
        if opc not in (FENCBR, FENCTR):
            cond = (w[1] >> 18) & 7
            ccswz = [(w[1] >> (21 + 2 * i)) & 3 for i in range(4)]
            cc_write = (w[0] >> 8) & 1
            prec = (w[0] >> 22) & 3
            if prec == 3:
                raise Unmodelled("precision 3")
            # the scale field is 3 bits (28-30); 4 is reserved, 5-7 divide
            if (w[2] >> 28) & 7:
                raise Unmodelled("output scale %d" % ((w[2] >> 28) & 7))
            n = ARITY.get(opc, 0)
            out_none = (w[0] >> 30) & 1
            mask = (w[0] >> 9) & 0xF
            if out_none and not cc_write:
                lanes = []
            elif opc in (DP2, DP3, DP4):
                lanes = list(range({DP2: 2, DP3: 3, DP4: 4}[opc]))
            else:
                lanes = [i for i in range(4) if mask & (1 << i)]
            # a scalar operand reads lane x of its (swizzled) source: RCP's
            # only source, DIV's divisor
            read_lanes = {1: [0] if opc == RCP else lanes, 2: [0] if opc == DIV else lanes, 3: lanes}
            srcs = [_read(w, slot, regs, inputs, const, read_lanes[slot]) for slot in range(1, n + 1)]
            for slot, v in enumerate(srcs, 1):
                for x in (v[i] for i in read_lanes[slot]):   # what the op reads, not what it writes
                    if prec == 1 and f16(x) != x:
                        raise Unmodelled("fp16 instruction on a non-fp16 value %r" % x)
                    if prec == 2 and not (-2.0 <= x < 2.0 and x * 1024 == int(x * 1024)):
                        raise Unmodelled("fx12 instruction on a non-fx12 value %r" % x)
            while len(srcs) < 3:
                srcs.append([0.0] * 4)
            res = _op(opc, *srcs)
            if (w[0] >> 31) & 1:
                res = [min(1.0, max(0.0, x)) for x in res]
            half = (w[0] >> 7) & 1
            if half or prec == 1:
                res = [f16(x) for x in res]
            # the test reads CC as it was BEFORE this instruction's own write
            commit = mask
            if cond != 7:
                commit = 0
                for i in range(4):
                    if mask & (1 << i):
                        v = cc[ccswz[i]]
                        if v is None:
                            raise Unmodelled("CC.%s tested before it was written" % "xyzw"[ccswz[i]])
                        if _CC_TEST[cond](v):
                            commit |= 1 << i
            if cc_write:
                for i in range(4):
                    if commit & (1 << i):
                        if res[i] != res[i]:
                            raise Unmodelled("NaN written to CC.%s" % "xyzw"[i])
                        cc[i] = res[i]
            if not out_none:
                regs.write(('H' if half else 'R', (w[0] >> 1) & 0x3F), commit, res)
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
    rows.append(('green: R0 = RCP(TEX0.x) broadcast',
                 _container(_ins(RCP, 0, 0xF, [_src(I)], sel=tex0, end=1)), [4.0] * 4))
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
    # condition codes: R0 = TEX1, CC = TEX0, R0(NE) = TEX0  ->  per-lane TEX0 != 0 ? TEX0 : TEX1
    def _pred(words, cond, swz=(0, 1, 2, 3)):
        words[1] = (words[1] & ~(0xFF << 18 | 7 << 18)) | (cond << 18)
        for i, c in enumerate(swz):
            words[1] |= c << (21 + 2 * i)
        return words
    tex1 = 0x5
    b = [1.25, 0.75, -1.0, 0.5]
    az = [0.25, 0.0, 1.0, 0.0]
    ccset = _ins(MOV, 63, 0xF, [_src(I)], sel=tex0, out_none=1)
    ccset[0] |= 1 << 8
    sel = lambda cond, swz=(0, 1, 2, 3): (_ins(MOV, 0, 0xF, [_src(I)], sel=tex1) + ccset
                                          + _pred(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0, end=1), cond, swz))
    rows.append(('green: per-lane select on CC (NE)', _container(sel(5)), [0.25, 0.75, 1.0, 0.5], az, b))
    rows.append(('green: per-lane select on CC (EQ)', _container(sel(2)), [1.25, 0.0, -1.0, 0.0], az, b))
    rows.append(('green: scalar select broadcast CC.x', _container(sel(5, (0, 0, 0, 0))), [0.25, 0.0, 1.0, 0.0], az, b))
    rows.append(('red: predicated MOV with CC never written',
                 _container(_ins(MOV, 0, 0xF, [_src(I)], sel=tex1)
                            + _pred(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0, end=1), 5)), None, az, b))
    # RCP precision is judged on the lane it READS (review: codex): fx12/fp16
    # RCP R0.y, TEX0.x with TEX0.x = 0.1 must refuse; masked and swizzled
    # reads of representable lanes stay green
    def _rcp(dst_mask, swz, prec):
        words = _ins(RCP, 0, dst_mask, [_src(I, swz=swz)], sel=tex0, end=1)
        words[0] |= prec << 22
        return _container(_ins(MOV, 0, 0xF, [_src(I)], sel=tex0) + words)
    tenth = [0.1, 0.5, 0.25, 1.0]
    rows.append(('red: fx12 RCP R0.y, TEX0.x (0.1 not fx12)', _rcp(0x2, 0x00, 2), None, tenth, b))
    rows.append(('red: fp16 RCP R0.y, TEX0.x (0.1 not fp16)', _rcp(0x2, 0x00, 1), None, tenth, b))
    rows.append(('green: fx12 RCP R0.y, TEX0.z (masked)', _rcp(0x2, 0xAA, 2), [0.1, 4.0, 0.25, 1.0], tenth, b))
    rows.append(('green: fp16 RCP R0.xw, TEX0.y (swizzled)', _rcp(0x9, 0x55, 1), [2.0, 0.5, 0.25, 2.0], tenth, b))
    rows.append(('green: fp32 RCP R0.y, TEX0.x', _rcp(0x2, 0x00, 0), [0.1, f32(1 / 0.1), 0.25, 1.0], tenth, b))
    # DIV: every lane of src0 over src1's x lane after swizzle
    def _div(swz1, prec=0, mask=0xF):
        words = _ins(DIV, 0, mask, [_src(I), _src(I, swz=swz1)], sel=tex0, end=1)
        words[0] |= prec << 22
        return _container(words)
    rows.append(('green: DIV R0, TEX0, TEX0.y (vector / scalar)', _div(0x55), [-0.5, 1.0, -2.0, -3.0]))
    # identity divisor swizzle: scalar src1.x gives a / a.x, a per-lane model would give 1s
    rows.append(('green: DIV R0, TEX0, TEX0 (divisor is lane x)', _div(0xE4), [1.0, -2.0, 4.0, 6.0]))
    rows.append(('green: DIV R0, TEX0, TEX0.w', _div(0xFF), [f32(x / 1.5) for x in a]))
    rows.append(('red: fx12 DIV, divisor TEX0.x = 0.1 not fx12', _div(0x00, prec=2), None, tenth, b))
    rows.append(('green: fx12 DIV R0, TEX0, TEX0.y (all fx12)', _div(0x55, prec=2), [-0.5, 1.0, -2.0, -3.0]))
    for scale in (1, 4):  # x2 and the reserved encoding (review: codex)
        words = _ins(MOV, 0, 0xF, [_src(I)], sel=tex0, end=1)
        words[2] |= scale << 28
        rows.append(('red: output scale encoding %d' % scale, _container(words), None))
    fails = 0
    for row in rows:
        name, blob, want = row[:3]
        ins = {'TEX0': row[3], 'TEX1': row[4]} if len(row) > 3 else {'TEX0': a}
        try:
            got = evaluate(blob, ins)
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
