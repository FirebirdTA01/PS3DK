"""`transpose(m)` moves VALUES between lanes; this checks the values.

The bucket (c) transpose rows used to assert that a container existed and
its ucode was not empty.  That is an assertion about the compiler having
run.  `transpose` is a permutation: every way of getting it wrong - a
row read as a column, one lane off, the whole call optimised away -
produces a container of exactly the right size with a non-empty ucode, and
every one of those passes an artifact check.

Two of these rows were worse than weak.  A `<< 'EOF'` in the shell script
had no terminator, so the heredoc swallowed two compiler invocations and
their assertions whole: `transpose_fp_double.cg` was never written and the
rows that looked like they ran had never run.

So this walks the emitted program with a small abstract interpreter and
requires the colour output to hold the exact dot products the source
names, over a matrix of literals (1..9) that makes every element
distinguishable from every other.  Against that fixture:

    mul(m, p)                        x = dp3(p, 1|2|3)  y = 4|5|6  z = 7|8|9
    mul(transpose(m), p)             x = dp3(p, 1|4|7)  y = 2|5|8  z = 3|6|9
    mul(transpose(transpose(m)), p)  the same as mul(m, p)

The third row is the reason the second and third fixtures both exist: the
reference folds the double transpose to the plain multiply and emits a
BYTE-IDENTICAL container for the two spellings, in both profiles.  We do
not fold it (32 fragment instructions against 8), which is a scheduling
gap and is carded separately - but a missing fold and a wrong permutation
look the same to a size check and completely different here.  This file
is indifferent to how many registers the program walks through and exact
about what ends up in the output.

IT REFUSES WHAT IT DOES NOT MODEL, in the shape insert_lane_check.py
established: these fixtures compile to MOV and DP3, so any other opcode,
any destination mode that changes a value (saturation, half bank, a
non-fp32 precision, a destination scale, a predicated or
condition-register-only write) and any operand mode outside plain
swizzle/negate/abs raises instead of being stepped over.  A checker that
skips an instruction it does not understand is a checker reporting a green
about a program it did not read.

Usage:  fp_transpose_check.py <ucode-dump> <row>;<row>;<row>

where each row is the three constants that lane's dot product must read,
in order, e.g. "1,4,7;2,5,8;3,6,9".  Lane w must be the literal 1.0.
Exits 0 when the output matches, 1 with the reason when it does not.
"""

import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")

from ucode_decode import CONST, INPUT, TEMP, groups          # noqa: E402

MOV, DP3 = 0x01, 0x05
MODELLED = (MOV, DP3)

COLOUR_OUT = 0                       # R0; the colour output shares the R0 slot
NAMES = "xyzw"


class Unmodelled(Exception):
    """The program contains something this checker must not silently skip."""


# Same list as insert_lane_check.py, and for the same reason: each of these
# changes what the shader paints while leaving the walk below looking
# correct.  A half-bank destination is the sharpest one here - the next
# instruction still reads "R2" and would be handed the fp32 register's
# value by an interpreter that ignored bit 7.
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
                "%s at instruction %d.  These fixtures emit none of these and "
                "each one changes the value; refusing rather than stepping "
                "over it." % (what, index))
    if (w[2] >> 28) & 7:                       # NVFX_FP_OP_DST_SCALE_SHIFT
        raise Unmodelled("a destination scale at instruction %d" % index)
    if ((w[1] >> 18) & 7) != 7:                # condition test, NVFX_COND_TR
        raise Unmodelled("a predicated write at instruction %d" % index)


def source(word, lane, block, regs, input_src, index):
    """The symbolic value this operand reads for destination lane 'lane'.

    The INPUT REGISTER's identity is per-INSTRUCTION (hw[0] bits 13..16),
    not per-operand: a fragment instruction has ONE input selector, so the
    operand word's register field means nothing for an input read.  It is
    read by the caller and passed in.
    """
    kind = word & 3
    reg = (word >> 2) & 0x3F
    swz = (word >> (9 + 2 * lane)) & 3
    if (word >> 8) & 1:                        # NVFX_FP_REG_SRC_HALF
        raise Unmodelled(
            "an operand at instruction %d reads the half register bank; "
            "these fixtures are fp32 throughout, so the program changed"
            % index)
    if kind == CONST:
        if block is None:
            raise Unmodelled(
                "an operand at instruction %d names the const block but its "
                "instruction carries no inline block" % index)
        term = ("k", block[swz])
    elif kind == INPUT:
        term = ("in", input_src, swz)
    elif kind == TEMP:
        term = regs.get((reg, swz), ("undef", reg, swz))
    else:
        raise Unmodelled("source register type %d at instruction %d"
                         % (kind, index))
    if (word >> 17) & 1:                       # NVFX_FP_REG_NEGATE
        term = ("neg", term)
    return term


def dot(a, b):
    """dp3 is symmetric in its operands; operand order must not decide it."""
    return ("dp3",) + tuple(sorted((tuple(a), tuple(b)), key=repr))


def evaluate(path):
    """Interpret the program, stopping where the hardware stops.

    PROGRAM_END (hw[0] bit 0) is semantics, not decoration: rows after the
    terminator are never reached, and a dump that never sets it is a dump
    of something other than a whole program.
    """
    rows = groups(path)
    regs = {}
    ended_at = None
    i = 0
    while i < len(rows):
        w = rows[i]
        if ended_at is not None:
            raise Unmodelled(
                "instruction %d follows PROGRAM_END at %d; the hardware "
                "never reaches it" % (i, ended_at))
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
                "and DP3, which is what these fixtures compile to; it "
                "refuses to step over an instruction it cannot evaluate "
                "rather than report a green about a program it did not read."
                % (opcode, i))
        reject_unmodelled(w, i)
        input_src = (w[0] >> 13) & 0xF         # NVFX_FP_OP_INPUT_SRC_SHIFT
        dst = (w[0] >> 1) & 0x3F
        mask = (w[0] >> 9) & 0xF
        if opcode == DP3:
            a = [source(w[1], lane, block, regs, input_src, i)
                 for lane in range(3)]
            b = [source(w[2], lane, block, regs, input_src, i)
                 for lane in range(3)]
            value = dot(a, b)
            for lane in range(4):
                if mask & (1 << lane):
                    regs[(dst, lane)] = value
        else:
            for lane in range(4):
                if mask & (1 << lane):
                    regs[(dst, lane)] = source(
                        w[1], lane, block, regs, input_src, i)
        if w[0] & 1:                           # NVFX_FP_OP_PROGRAM_END
            ended_at = i
        i += 1 + (1 if block is not None else 0)
    if ended_at is None:
        raise Unmodelled("the dump carries no PROGRAM_END, so it is not a "
                         "whole program")
    return regs


def parse_expected(spec):
    rows = [r for r in spec.split(";") if r.strip()]
    if len(rows) != 3:
        raise SystemExit("expected three rows separated by ';', got %r" % spec)
    return [[float(v) for v in row.split(",")] for row in rows]


def approx(term, value):
    return (isinstance(term, tuple) and term[0] == "k"
            and abs(term[1] - value) < 1e-6)


def describe(term):
    if not isinstance(term, tuple):
        return repr(term)
    if term[0] == "k":
        return "%g" % term[1]
    if term[0] == "in":
        return "in%d.%s" % (term[1], NAMES[term[2]])
    if term[0] == "neg":
        return "-" + describe(term[1])
    if term[0] == "dp3":
        return "dp3(%s , %s)" % ("|".join(describe(t) for t in term[1]),
                                 "|".join(describe(t) for t in term[2]))
    return "%s(%s)" % (term[0], ", ".join(describe(t) for t in term[1:]))


def check(path, spec):
    expected = parse_expected(spec)
    regs = evaluate(path)
    for lane in range(4):
        if (COLOUR_OUT, lane) not in regs:
            return ("the colour output's %s lane is never written"
                    % NAMES[lane])
    if not approx(regs[(COLOUR_OUT, 3)], 1.0):
        return ("the colour output's w lane is %s, not the literal 1.0 the "
                "source returns" % describe(regs[(COLOUR_OUT, 3)]))
    for lane in range(3):
        term = regs[(COLOUR_OUT, lane)]
        if not (isinstance(term, tuple) and term[0] == "dp3"):
            return ("the colour output's %s lane is %s, not a dot product; "
                    "mul(matrix, vector) is three dp3s"
                    % (NAMES[lane], describe(term)))
        want = expected[lane]
        # One operand must be the input's x, y and z in that order, and the
        # other the three matrix elements.  Which of the two operand slots
        # holds which is the emitter's business, and dp3 is symmetric.
        for varying, consts in (term[1], term[2]), (term[2], term[1]):
            if not all(isinstance(t, tuple) and t[0] == "in" for t in varying):
                continue
            if [t[2] for t in varying] != [0, 1, 2]:
                continue
            if len(set(t[1] for t in varying)) != 1:
                continue
            if all(approx(t, want[k]) for k, t in enumerate(consts)):
                break
        else:
            return ("the colour output's %s lane is %s; expected the input's "
                    "xyz dotted with %s"
                    % (NAMES[lane], describe(term),
                       "|".join("%g" % v for v in want)))
    return None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[-4])
    path, spec = sys.argv[1], sys.argv[2]
    try:
        reason = check(path, spec)
    except Unmodelled as exc:
        print("REFUSED %s: %s" % (path, exc))
        return 1
    if reason:
        print("FAIL %s: %s" % (path, reason))
        return 1
    print("ok %s: colour output is %s" % (path, spec))
    return 0


if __name__ == "__main__":
    sys.exit(main())
