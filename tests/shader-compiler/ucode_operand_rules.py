"""Decode an NV40 fragment ucode dump and check the two operand rules.

Used by general-operand-legalisation-test.sh.  Kept as a file rather than
inlined in the shell script because the walk below is the part that is
easy to get wrong, and a decoder that is wrong reports a clean program as
clean for the wrong reason.

The two rules (t_40dd8159):

  1. ONE input-source selector per instruction.  The selector lives in
     hw[0] bits 13..16, on the INSTRUCTION, so two input-typed sources
     necessarily read the SAME varying.  Legal when the source named the
     same one - which is why this is only ever asserted on a fixture whose
     source names two different varyings.

  2. ONE inline constant block per instruction.  A block is 16 bytes of
     data appended after the instruction that names it; naming two
     constants means the second block is decoded as an instruction.
"""

import re
import sys


def unswap(v):
    """Words are printed as the hardware stores them: halfwords swapped."""
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


# Source register type: low two bits of each source word
# (NVFX_FP_REG_TYPE_*).
TEMP, INPUT, CONST = 0, 1, 2


def walk(path):
    """Yield (index, words, const_source_count) per INSTRUCTION.

    Advances past inline const blocks, so literals are never read as
    opcodes.
    """
    groups = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
            if not m:
                continue
            words = [unswap(int(x, 16)) for x in m.group(2).split()]
            if len(words) == 4:
                groups.append(words)
    i = 0
    while i < len(groups):
        w = groups[i]
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == CONST)
        yield i, w, consts
        i += 1 + (1 if consts else 0)


def check(path):
    name = path.replace("\\", "/").split("/")[-1]
    seen = 0
    comparisons = 0
    for idx, w, consts in walk(path):
        opcode = (w[0] >> 24) & 0x3F
        if opcode == 0:
            continue
        seen += 1
        if 0x0A <= opcode <= 0x0F:      # SLT SGE SLE SGT SNE SEQ
            comparisons += 1
        inputs = sum(1 for s in (1, 2, 3) if (w[s] & 3) == INPUT)
        if inputs > 1:
            raise SystemExit(
                "FAIL: %s instruction %d (opcode 0x%02X) has %d input-typed "
                "sources.  An NV40 fragment instruction has ONE input "
                "selector, so both read the same varying - and this "
                "fixture's operands are two different varyings "
                "(t_40dd8159 / t_e89cd261)." % (name, idx, opcode, inputs))
        if consts > 1:
            raise SystemExit(
                "FAIL: %s instruction %d (opcode 0x%02X) names %d inline "
                "constants.  One block is appended per instruction, so the "
                "second is decoded as an instruction and the operand reads "
                "the first (t_40dd8159)." % (name, idx, opcode, consts))
    if seen == 0:
        raise SystemExit(
            "FAIL: %s decoded no instructions, so both rules were checked "
            "against an empty program - blank is not clean" % name)
    if comparisons == 0:
        raise SystemExit(
            "FAIL: %s emitted no comparison instruction (opcodes 0x0A-0x0F); "
            "the fixture no longer exercises what it was written for" % name)


def main():
    for path in sys.argv[1:]:
        check(path)
    print("general-operand-legalisation: every instruction addresses one "
          "input and carries one inline const, on both fixtures")


if __name__ == "__main__":
    main()
