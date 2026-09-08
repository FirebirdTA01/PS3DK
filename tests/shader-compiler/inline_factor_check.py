"""The program's output is exactly FACTOR times its one input, lane by lane.

For the rebound rows of user-function-inline-control-flow-test.sh
(t_3af598c8): main returns 2*G or 6*p after an inlined helper writes the
file-scope name.  A row that asserts "a MUL reads the input" plus "the bytes
of 2.0 are somewhere in the container" is satisfied by a program that reads
the const block's ZERO lane (swizzle .yyyy over {2,0,0,0}) or that negates
the factor - codex built both containers and all four earlier assertions
passed on each.  So this walks the program with the decoded operands and
computes the value: input lanes (0.25, -0.5, 0.75, 2.0), the const block's
own floats through each operand's swizzle, negate and abs, MOV and MUL.  The
factors here (2, 3, 6) and the inputs are exact in binary, so the comparison
is exact.

IT REFUSES WHAT IT DOES NOT MODEL: any opcode but MOV/MUL (FENCBR, which
writes nothing, is stepped over), half-bank operands or destinations,
saturation, scale, predication, condition writes, a program without
PROGRAM_END or with rows after it.  A checker that steps over an
instruction it cannot evaluate reports a green about a program it did not
read (insert_lane_check.py learned that in three rounds).

The vertex side delegates the arithmetic to vp_special_output_check.inspect()
but NOT the envelope: inspect() never reads PROGRAM_END, the condition test,
the scalar slot, abs or saturate (review: codex - clearing LAST on a valid
container still passed), so vp_envelope() refuses those first.

usage: inline_factor_check.py <container> fp|vp <factor>
           [--control lane|negate|no-end|early-end]

--control mutates the CONTAINER BYTES - the first MUL's const operand (the
lane it reads, or its sign), or PROGRAM_END (cleared on the last row, or set
on the first) - writes <container>.<control>, and then REQUIRES the check to
reject it: exit 0 means the mutant was rejected, exit 1 means the predicate
is too weak to see it.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
import fp_sources                                          # noqa: E402
from vp_special_output_check import inspect as vp_inspect  # noqa: E402

INPUT = [0.25, -0.5, 0.75, 2.0]
FP_MOV, FP_MUL, FP_FENCBR = 0x01, 0x02, 0x3E
VP_MUL = 2


class Unmodelled(Exception):
    pass


def f32(word):
    return struct.unpack("<f", struct.pack("<I", word & 0xFFFFFFFF))[0]


# ----------------------------------------------------------------- fragment
def fp_operand(w, slot, lane, block, regs):
    s = fp_sources.source(w, slot)
    if s["half"]:
        raise Unmodelled("slot %d reads the half bank" % s["slot"])
    swz = (s["swizzle"] >> (2 * lane)) & 3
    if s["type"] == fp_sources.CONST:
        if block is None:
            raise Unmodelled("a const operand with no inline block")
        v = f32(block[swz])
    elif s["type"] == fp_sources.INPUT:
        v = INPUT[swz]
    elif s["type"] == fp_sources.TEMP:
        key = (s["reg"], swz)
        if key not in regs:
            raise Unmodelled("R%d.%s read before it is written"
                             % (s["reg"], "xyzw"[swz]))
        v = regs[key]
    else:
        raise Unmodelled("source type %d" % s["type"])
    if s["abs"]:
        v = abs(v)
    if s["negate"]:
        v = -v
    return v


FP_MODES = ((31, 1, "saturation"), (30, 1, "OUT_NONE"),
            (8, 1, "condition write"), (7, 1, "half destination"),
            (22, 3, "non-fp32 precision"))


def fp_evaluate(blob):
    words = fp_sources.ucode_words(blob)
    regs, inputs_read, ended = {}, set(), False
    for n, (w, block) in enumerate(fp_sources.instructions(words)):
        if ended:
            raise Unmodelled("instruction %d follows PROGRAM_END" % n)
        op = (w[0] >> 24) & 0x3F
        if op == FP_FENCBR:
            ended = bool(w[0] & 1)
            continue
        if op not in (FP_MOV, FP_MUL):
            raise Unmodelled("opcode 0x%02X at instruction %d (MOV/MUL only)"
                             % (op, n))
        for shift, mask, what in FP_MODES:
            if (w[0] >> shift) & mask:
                raise Unmodelled("%s at instruction %d" % (what, n))
        if (w[2] >> 28) & 7:
            raise Unmodelled("destination scale at instruction %d" % n)
        if ((w[1] >> 18) & 7) != 7:
            raise Unmodelled("predicated write at instruction %d" % n)
        input_src = (w[0] >> 13) & 0xF
        dst, mask = (w[0] >> 1) & 0x3F, (w[0] >> 9) & 0xF
        arity = 1 if op == FP_MOV else 2
        for slot in range(1, arity + 1):
            if (w[slot] & 3) == fp_sources.INPUT:
                inputs_read.add(fp_sources.INPUT_NAME.get(
                    input_src, "input%d" % input_src))
        for lane in range(4):
            if not mask & (1 << lane):
                continue
            a = fp_operand(w, 1, lane, block, regs)
            if op == FP_MUL:
                a *= fp_operand(w, 2, lane, block, regs)
            regs[(dst, lane)] = a
        if w[0] & 1:
            ended = True
    if not ended:
        raise Unmodelled("no PROGRAM_END")
    return [regs.get((0, lane)) for lane in range(4)], inputs_read


def fp_mutate(blob, control):
    """lane/negate: flip the FIRST MUL's const operand (-> .yyyy, or its sign).
    no-end: clear PROGRAM_END on the last row.  early-end: set it on the first."""
    ucode_off = struct.unpack_from(">8I", blob, 0)[7]
    words = fp_sources.ucode_words(blob)
    if control in ("no-end", "early-end"):
        # instructions() steps over const blocks, so map each instruction back
        # to its word offset; the last ROW may be a const block, not an instruction
        offs, i = [], 0
        for w, block in fp_sources.instructions(words):
            offs.append(i)
            i += 4 + (4 if block is not None else 0)
        target = offs[-1] if control == "no-end" else offs[0]
        new = (words[target] & ~1) if control == "no-end" else (words[target] | 1)
        out = bytearray(blob)
        struct.pack_into(">I", out, ucode_off + target * 4, fp_sources.unswap(new))
        return bytes(out)
    i = 0
    while i + 4 <= len(words):
        w = words[i:i + 4]
        op = (w[0] >> 24) & 0x3F
        slots = [s for s in (1, 2) if (w[s] & 3) == fp_sources.CONST]
        if op == FP_MUL and slots:
            slot = slots[0]
            if control == "lane":
                new = (w[slot] & ~(0xFF << 9)) | (0x55 << 9)
            else:
                new = w[slot] ^ (1 << 17)
            out = bytearray(blob)
            struct.pack_into(">I", out, ucode_off + (i + slot) * 4,
                             fp_sources.unswap(new))
            return bytes(out)
        has_const = any((w[s] & 3) == fp_sources.CONST for s in (1, 2, 3))
        i += 4 + (4 if has_const else 0)
    raise Unmodelled("no MUL with a const operand to mutate")


# ------------------------------------------------------------------- vertex
VP_MOV = 1
# nv40_vertprog.h, DWORD 0: the modifiers and modes that change a value
# without changing the vector opcode the evaluator reads.
VP_DWORD0_MODES = ((29, 1, "condition update (bit 29)"), (27, 1, "indexed input"),
                   (26, 1, "saturation"), (23, 1, "src2 abs"), (22, 1, "src1 abs"),
                   (21, 1, "src0 abs"), (14, 1, "condition update (bit 14)"),
                   (13, 1, "condition test"))


def vp_envelope(blob):
    """Fail closed on what vp_special_output_check.inspect() does not look at.

    inspect() evaluates the VECTOR opcode and its three sources and nothing
    else: it does not read NVFX_VP_INST_LAST, the condition test, the scalar
    slot, abs or saturate (review: codex - clearing LAST on the valid
    rebound container still passed).  So every instruction's envelope is
    checked HERE, before delegation, and the walk requires LAST on the
    final instruction and on no other.
    """
    size, off = struct.unpack_from(">2I", blob, 24)
    if size == 0 or size % 16 or off + size > len(blob):
        raise Unmodelled("VP ucode region is not a whole number of instructions")
    count = size // 16
    for n in range(count):
        w = struct.unpack_from(">4I", blob, off + n * 16)
        vop = (w[1] >> 22) & 0x1F
        if vop not in (VP_MOV, VP_MUL):
            raise Unmodelled("vector opcode %d at instruction %d (MOV/MUL only)" % (vop, n))
        # A vector-only instruction carries SCA_RESULT with scalar dest 63 and
        # an EMPTY scalar writemask (nv40_vp_assembler.cpp:104-105); a scalar
        # write is a scalar opcode or a non-empty scalar mask, not that bit.
        sop = (w[1] >> 27) & 0x1F
        if sop or (w[3] >> 17) & 0xF:
            raise Unmodelled("a scalar-slot opcode/write at instruction %d" % n)
        for shift, mask, what in VP_DWORD0_MODES:
            if (w[0] >> shift) & mask:
                raise Unmodelled("%s at instruction %d" % (what, n))
        if (w[3] >> 1) & 1:
            raise Unmodelled("relative const addressing at instruction %d" % n)
        last = w[3] & 1
        if last and n != count - 1:
            raise Unmodelled("PROGRAM_END at instruction %d of %d: the hardware "
                             "never reaches the rows after it" % (n, count))
        if not last and n == count - 1:
            raise Unmodelled("the final instruction does not carry PROGRAM_END")


def vp_evaluate(blob):
    vp_envelope(blob)
    # inspect() evaluates MOV/MUL from the three sources; these fixtures read IN0 only
    outputs, _, _, _, _ = vp_inspect(blob, {0: INPUT})
    if 0 not in outputs:
        raise Unmodelled("o0 is never written")
    return outputs[0], {"IN0"}


def vp_mutate(blob, control):
    size, off = struct.unpack_from(">2I", blob, 24)
    out = bytearray(blob)
    if control in ("no-end", "early-end"):
        pos = off + size - 16 if control == "no-end" else off
        w3 = struct.unpack_from(">I", blob, pos + 12)[0]
        w3 = (w3 & ~1) if control == "no-end" else (w3 | 1)
        struct.pack_into(">I", out, pos + 12, w3)
        return bytes(out)
    for pos in range(off, off + size, 16):
        w = list(struct.unpack_from(">4I", blob, pos))
        op = (w[1] >> 22) & 31
        s1 = (w[2] >> 6) & 0x1FFFF
        if op == VP_MUL and (s1 & 3) == 3:
            if control == "lane":
                s1 = (s1 & ~(0xFF << 8)) | (0x55 << 8)
            else:
                s1 ^= 1 << 16
            w[2] = (w[2] & ~(0x1FFFF << 6)) | (s1 << 6)
            struct.pack_into(">4I", out, pos, *w)
            return bytes(out)
    raise Unmodelled("no MUL with a const operand to mutate")


# -------------------------------------------------------------------- check
def check(blob, profile, factor):
    out, inputs_read = (fp_evaluate if profile == "fp" else vp_evaluate)(blob)
    if len(inputs_read) != 1:
        return "reads %d input registers (%s), expected exactly one" % (
            len(inputs_read), ", ".join(sorted(inputs_read)))
    want = [factor * x for x in INPUT]
    bad = [(lane, out[lane], want[lane]) for lane in range(4)
           if out[lane] != want[lane]]
    if bad:
        return "output is not %g * input: " % factor + "; ".join(
            "lane %s = %s, expected %g"
            % ("xyzw"[lane], "unwritten" if got is None else "%g" % got, exp)
            for lane, got, exp in bad)
    return None


def main(argv):
    if len(argv) not in (4, 6) or argv[2] not in ("fp", "vp"):
        raise SystemExit(__doc__)
    path, profile, factor = argv[1], argv[2], float(argv[3])
    blob = open(path, "rb").read()
    try:
        if len(argv) == 6:
            control = argv[5]
            mutant = (fp_mutate if profile == "fp" else vp_mutate)(blob, control)
            open(path + "." + control, "wb").write(mutant)
            # A mutant the walk REFUSES is rejected too - refusal is the
            # fail-closed half of the predicate, and the END controls land there.
            try:
                problem = check(mutant, profile, factor)
            except (Unmodelled, fp_sources.ContainerError, AssertionError) as exc:
                problem = "unmodelled: %s" % exc
            if problem is None:
                sys.stderr.write("FAIL: the %s control still passes the value check\n"
                                 % control)
                return 1
            print("control %s rejected: %s" % (control, problem))
            return 0
        problem = check(blob, profile, factor)
    except (Unmodelled, fp_sources.ContainerError, AssertionError) as exc:
        sys.stderr.write("FAIL: unmodelled: %s\n" % exc)
        return 1
    if problem:
        sys.stderr.write("FAIL: %s\n" % problem)
        return 1
    print("output = %g * input on every lane" % factor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
