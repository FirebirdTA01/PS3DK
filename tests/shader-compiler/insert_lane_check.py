"""Every lane of the colour output carries the VALUE the fixture computes.

t_856689b2: the general path's VecInsert lowering forced the scalar
source's swizzle to lane 0.  That is right for a literal - lane 0 of the
const block - and wrong for a lane extract, whose lane resolve() had
already selected.  So `color.y = lit.y` emitted `MOV R0.y, R16.x`, the red
channel broadcast into green and blue, on four shaders whose reference
paints three distinct channels.

TWO EARLIER VERSIONS OF THIS CHECK WERE TOO WEAK, in opposite ways, and
both failures are why it is written this way now.

The FIRST asserted the SHAPE the compiler happened to emit: three
single-lane MOVs, masks 0x1, 0x2 and 0x4, each from a temp.  06baa4eb then
compacted them into one three-lane `MOV R0.xyz <- R1.xyz` - the reference's
own shape, value unchanged - and the check stopped matching anything at
all, reporting "masks found: <empty>.  Nothing was checked".  It sat red on
the integration branch for a compiler that was behaving correctly.

The SECOND asserted the LANE INDEX only: for each written lane, the last
writer's src0 must read that same lane.  codex broke it in review, by
measurement rather than by argument: rewrite that MOV's src0 from the
computed temp to the ORIGINAL INPUT, keeping the xyz selectors, and the
check still passes - while the shader now paints (.8, .4, .2) instead of
(.4, .1, .025), because the multiply has been bypassed entirely.  Matching
lane indices proves the SELECTOR is right and says nothing about WHOSE lane
it is.

So this version asserts the VALUE.  It walks the program with a small
abstract interpreter - input reads, the inline const block's actual floats,
multiplies, negation - and requires the colour output to hold exactly

    x = in.x * 0.5    y = in.y * 0.25    z = in.z * 0.125    w = in.w

from ONE input register.  A wrong lane fails it.  A bypassed multiply fails
it.  A wrong constant fails it.  A sign flip fails it.  And it is
indifferent to how the compiler gets there: one MOV per lane, one
three-lane MOV, or a later fold that writes the output straight from the
multiply all satisfy it, because all three compute the same value.

IT REFUSES WHAT IT DOES NOT MODEL.  This fixture compiles to MOV and MUL.
Any other opcode raises rather than being skipped: a checker that steps
over an instruction it does not understand is a checker reporting a green
about a program it did not read.  If the lowering legitimately starts
emitting MAD here, extending this file is a deliberate act by someone who
has looked at the new shape.

THIS ASSERTION IS ABOUT ITS FIXTURE.  fp_insert_lanes_of_vector_f writes
lit.x, lit.y and lit.z into x, y and z of a copy of v, where
lit = v * float4(0.5, 0.25, 0.125, 1.0).  The expected terms above are that
source and nothing more general.  A shader is only evidence for the rule
its own text states.
"""

import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")

from ucode_decode import CONST, INPUT, TEMP, groups          # noqa: E402

MOV, MUL = 0x01, 0x02
MODELLED = (MOV, MUL)

COLOUR_OUT = 0                       # R0; the colour output shares the R0 slot
NAMES = "xyzw"

# lit = v * float4(0.5, 0.25, 0.125, 1.0); c = v; c.xyz = lit.xyz
# lane w is a plain copy of the input, so it carries no scale.
EXPECTED_SCALE = (0.5, 0.25, 0.125, None)


class Unmodelled(Exception):
    """The program contains something this checker must not silently skip."""


# EVERY MODE THAT CHANGES A VALUE AND IS NOT MODELLED IS REFUSED, BY NAME.
# codex found three of these in review, each a mutation of the real words
# that changed what the shader paints while the interpreter reported a
# green: destination saturation, a half-bank destination that the next
# instruction still reads as R, and (with the input-register fix in
# source()) a per-instruction input selector pointing at another varying.
# Modelling them is not needed here - this fixture emits none of them - but
# stepping over them silently is exactly the hole the whole guard is about.
INSTRUCTION_MODES = (
    (31, 1, "destination saturation (NVFX_FP_OP_OUT_SAT)"),
    (30, 1, "a condition-register-only write (NV40_FP_OP_OUT_NONE)"),
    (8, 1, "a condition-code write (NVFX_FP_OP_COND_WRITE_ENABLE)"),
    (7, 1, "a half-bank destination (NVFX_FP_OP_OUT_REG_HALF)"),
    (22, 3, "a non-fp32 arithmetic precision"),
)


def reject_unmodelled(w, index):
    for shift, mask, what in INSTRUCTION_MODES:
        if (w[0] >> shift) & mask:
            raise Unmodelled(
                "%s at instruction %d.  This fixture emits none of these and "
                "each one changes the value; refusing rather than stepping "
                "over it." % (what, index))
    if (w[2] >> 28) & 7:                       # NVFX_FP_OP_DST_SCALE_SHIFT
        raise Unmodelled("a destination scale at instruction %d" % index)
    if ((w[1] >> 18) & 7) != 7:                # condition test, NVFX_COND_TR
        raise Unmodelled("a predicated write at instruction %d" % index)


def source(word, lane, block, regs, absolute, input_src):
    """The symbolic value this operand reads for destination lane 'lane'.

    TWO FIELDS THIS OPERAND WORD DOES NOT CARRY, both found by codex in
    review after an earlier version read them out of the wrong place:

      the INPUT REGISTER's identity is per-INSTRUCTION, hw[0] bits 13..16
      (NVFX_FP_OP_INPUT_SRC_SHIFT) - a fragment instruction has ONE input
      selector, so the operand word's register index means nothing for an
      input.  Reading it there let a mutation point the multiply at COL1
      while the copy still read TC0 (this fixture's varying) - RGB from
      one attribute and w from another - and the check passed.

      the ABSOLUTE-VALUE flag lives in hw[1] bit 29 for src0 and hw[2] bit
      18 for src1 (nv40_fp_assembler.cpp).

    Both are read by the caller and passed in.
    """
    kind = word & 3
    index = (word >> 2) & 0x3F
    swz = (word >> (9 + 2 * lane)) & 3
    if (word >> 8) & 1:                        # NVFX_FP_REG_SRC_HALF
        raise Unmodelled("an operand reads the half register bank; this "
                         "fixture is fp32 throughout, so the program changed")
    if kind == CONST:
        if block is None:
            raise Unmodelled("an operand names the const block but its "
                             "instruction carries no inline block")
        term = ("k", block[swz])
    elif kind == INPUT:
        term = ("in", input_src, swz)
    elif kind == TEMP:
        term = regs.get((index, swz), ("undef", index, swz))
    else:
        raise Unmodelled("source register type %d" % kind)
    if absolute:
        term = ("abs", term)
    if (word >> 17) & 1:                       # NVFX_FP_REG_NEGATE
        term = ("neg", term)
    return term


def product(a, b):
    """Multiplication is commutative; operand order must not decide this."""
    return ("mul",) + tuple(sorted((a, b), key=repr))


def evaluate(path):
    """Interpret the program, stopping where the hardware stops.

    PROGRAM_END is hw[0] bit 0 and it is part of the semantics, not
    decoration: codex's third round set END on the FIRST instruction of the
    real dump and an earlier version of this walk still read the multiply
    and the final colour write that the hardware would never reach - the
    output's x lane is not even initialised there.  So the walk stops at
    END, and a dump with no terminator, or with executable rows after one,
    is refused rather than interpreted.
    """
    rows = groups(path)
    regs = {}
    ended_at = None
    i = 0
    while i < len(rows):
        w = rows[i]
        block = None
        if any((w[s] & 3) == CONST for s in (1, 2, 3)):
            if i + 1 >= len(rows):
                raise Unmodelled("an instruction names an inline const block "
                                 "the dump does not contain")
            block = [struct.unpack("<f", struct.pack("<I", v))[0]
                     for v in rows[i + 1]]
        opcode = (w[0] >> 24) & 0x3F
        if opcode not in MODELLED:
            raise Unmodelled(
                "opcode 0x%02X at instruction %d.  This checker models MOV "
                "and MUL, which is what this fixture compiles to; it refuses "
                "to step over an instruction it cannot evaluate rather than "
                "report a green about a program it did not read."
                % (opcode, i))
        reject_unmodelled(w, i)
        abs0 = (w[1] >> 29) & 1                # NVFX_FP_OP_SRC0_ABS in hw[1]
        abs1 = (w[2] >> 18) & 1                # NVFX_FP_OP_SRC1_ABS in hw[2]
        input_src = (w[0] >> 13) & 0xF         # NVFX_FP_OP_INPUT_SRC_SHIFT
        dst = (w[0] >> 1) & 0x3F
        mask = (w[0] >> 9) & 0xF
        for lane in range(4):
            if not mask & (1 << lane):
                continue
            first = source(w[1], lane, block, regs, abs0, input_src)
            if opcode == MOV:
                regs[(dst, lane)] = first
            else:
                regs[(dst, lane)] = product(
                    first, source(w[2], lane, block, regs, abs1, input_src))
        i += 1 + (1 if block is not None else 0)
        if w[0] & 1:                           # NVFX_FP_OP_PROGRAM_END
            ended_at = i
            break

    if ended_at is None:
        raise Unmodelled("the program has no PROGRAM_END terminator, so "
                         "where the hardware stops is not knowable from "
                         "these words")
    if ended_at != len(rows):
        raise Unmodelled(
            "%d row(s) follow PROGRAM_END at instruction %d.  The hardware "
            "never reaches them; interpreting them would credit the program "
            "with work it does not do."
            % (len(rows) - ended_at, ended_at - 1))
    return regs


def expected(input_index):
    want = {}
    for lane, scale in enumerate(EXPECTED_SCALE):
        read = ("in", input_index, lane)
        want[lane] = read if scale is None else product(read, ("k", scale))
    return want


def show(term):
    kind = term[0]
    if kind == "in":
        return "in%d.%s" % (term[1], NAMES[term[2]])
    if kind == "k":
        return "%g" % term[1]
    if kind == "neg":
        return "-%s" % show(term[1])
    if kind == "mul":
        return "(%s * %s)" % (show(term[1]), show(term[2]))
    return "%s%r" % (kind, term[1:])


def inputs_of(term, seen):
    if term[0] == "in":
        seen.add(term[1])
    elif term[0] == "neg":
        inputs_of(term[1], seen)
    elif term[0] == "mul":
        inputs_of(term[1], seen)
        inputs_of(term[2], seen)


def check(path):
    regs = evaluate(path)
    got = dict((lane, regs.get((COLOUR_OUT, lane))) for lane in range(4))
    missing = [lane for lane in range(4) if got[lane] is None]
    if missing:
        return ("the colour output's %s lane is never written, so nothing was "
                "checked" % ", ".join(NAMES[lane] for lane in missing))

    seen = set()
    for lane in range(4):
        inputs_of(got[lane], seen)
    if len(seen) != 1:
        return ("the colour output reads %d input registers (%s); this "
                "fixture has one varying"
                % (len(seen), ", ".join(str(i) for i in sorted(seen))))

    want = expected(seen.pop())
    wrong = [lane for lane in range(4) if got[lane] != want[lane]]
    if wrong:
        return ("the colour output does not carry the value the shader "
                "computes - "
                + "; ".join("lane %s is %s, expected %s"
                            % (NAMES[lane], show(got[lane]), show(want[lane]))
                            for lane in wrong)
                + ".  lit = v * float4(0.5, 0.25, 0.125, 1.0) and "
                  "c.xyz = lit.xyz, so lane L must be v.L scaled by L's own "
                  "factor (t_856689b2).")
    return None


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: insert_lane_check.py <ucode-dump>")
    try:
        problem = check(sys.argv[1])
    except Unmodelled as exc:
        sys.stderr.write("FAIL: %s\n" % exc)
        raise SystemExit(1)
    if problem:
        sys.stderr.write("FAIL: %s\n" % problem)
        raise SystemExit(1)
    print("insert_lane_check: every colour lane carries its computed value")


if __name__ == "__main__":
    main()
