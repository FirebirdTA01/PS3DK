"""Evaluate a straight-line NV40 fragment program numerically (binary32).

The existing value checkers are fixture-specific.  This one runs any program
built only from the ops it models, on caller-supplied inputs, and returns the
colour output, so two containers (ours and the reference's) can be compared
by VALUE when their bytes differ.  It refuses, with a reason, anything it does
not model rather than stepping over it:

  - predicated instructions (condition test other than TR), branches, KIL;
  - reading an R register after one of its H halves was written, or an H
    register after its R register was written (H2k/H2k+1 alias Rk: the bit
    layout of that aliasing is not modelled, so a mixed read is refused);
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


def _read(w, slot, regs, inputs, const):
    s = source(w, slot)
    if s["type"] == TEMP:
        key = ('H' if s["half"] else 'R', s["reg"])
        if key in regs.get('stale', set()):
            raise Unmodelled("%s%d read after its aliased bank was written" % key)
        v = regs.get(key, [0.0, 0.0, 0.0, 0.0])
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
    swz = s["swizzle"]
    out = [v[(swz >> (2 * i)) & 3] for i in range(4)]
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
    regs = {}
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
            srcs = [_read(w, slot, regs, inputs, const) for slot in range(1, n + 1)]
            for v in srcs:
                for x in v:
                    if prec == 1 and f16(x) != x:
                        raise Unmodelled("fp16 instruction on a non-fp16 value %r" % x)
                    if prec == 2 and not (-2.0 <= x < 2.0 and x * 1024 == int(x * 1024)):
                        raise Unmodelled("fx12 instruction on a non-fx12 value %r" % x)
            while len(srcs) < 3:
                srcs.append([0.0] * 4)
            res = _op(opc, *srcs)
            if (w[0] >> 31) & 1:
                res = [min(1.0, max(0.0, x)) for x in res]
            if not (w[0] >> 30) & 1:
                half = (w[0] >> 7) & 1
                reg = (w[0] >> 1) & 0x3F
                key = ('H' if half else 'R', reg)
                if half:
                    res = [f16(x) for x in res]
                cur = regs.get(key, [0.0, 0.0, 0.0, 0.0])
                mask = (w[0] >> 9) & 0xF
                regs[key] = [res[i] if mask & (1 << i) else cur[i] for i in range(4)]
                stale = regs.setdefault('stale', set())
                stale.discard(key)
                if half:
                    stale.add(('R', reg // 2))
                else:
                    stale.update({('H', 2 * reg), ('H', 2 * reg + 1)})
        if w[0] & 1:
            ended = True
            break
    if not ended:
        raise Unmodelled("no PROGRAM_END")
    prog = struct.unpack_from(">8I", blob, 0)[5]
    key = ('H', 0) if blob[prog + 19] else ('R', 0)
    if key in regs.get('stale', set()):
        raise Unmodelled("output %s%d was overwritten through its aliased bank" % key)
    return regs.get(key, [0.0, 0.0, 0.0, 0.0])
