"""No instruction in the program is predicated: every write is unconditional.

For the then-only rows of user-function-inline-control-flow-test.sh
(t_7396e0c2): a then-only local that dies before the join must leave NO
Select behind.  A row that asserts this on fp_sources' text cannot see it:
that decoder prints neither the condition test (hw[1] bits 18..20) nor the
condition-code write (hw[0] bit 8), so a container whose output MOV carries
NVFX_COND_FL - a write that never executes - decodes to the same text as an
unconditional one (review: codex, block-then-param-cond-false.bin passed
the suite).  This reads the fields.

An instruction is predicated when its condition test is not NVFX_COND_TR
(7), or it writes the condition code (a compare feeding a later predicate).
OUT_NONE rows with a real opcode (a condition-register-only write, the
predicate half of a lowered select) are predication too; FENCBR's OUT_NONE
is not (it writes nothing by design and carries no condition).

usage: predication_check.py <container> [--control]

--control flips the condition test of the FIRST instruction that writes R0
to NVFX_COND_FL in the container bytes, writes <container>.condfl, and then
REQUIRES this check to reject it: exit 0 means the mutant was rejected, exit
1 means the predicate is too weak to see it.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
import fp_sources  # noqa: E402

FENCBR = 0x3E
COND_TR = 7


def cond_test(w):
    return (w[1] >> 18) & 7


def problems(blob):
    words = fp_sources.ucode_words(blob)
    found = []
    for n, (w, _block) in enumerate(fp_sources.instructions(words)):
        op = (w[0] >> 24) & 0x3F
        if op == FENCBR:
            continue
        if cond_test(w) != COND_TR:
            found.append("instruction %d has condition test %d (not TR): a predicated write"
                         % (n, cond_test(w)))
        if (w[0] >> 8) & 1:
            found.append("instruction %d writes the condition code: a predicate is being formed" % n)
        if (w[0] >> 30) & 1:
            found.append("instruction %d is OUT_NONE with opcode 0x%02X: a condition-only write" % (n, op))
    return found


def mutate(blob):
    """Flip the first R0-writing instruction's condition test to FL (0).

    Returns (mutant, index) so the caller can require the rejection to name
    THIS instruction for THIS reason - a control rejected for an unrelated
    reason (a cc-write elsewhere, say) proves nothing (review: codex).
    """
    ucode_off = struct.unpack_from(">8I", blob, 0)[7]
    words = fp_sources.ucode_words(blob)
    i = 0
    for n, (w, block) in enumerate(fp_sources.instructions(words)):
        op = (w[0] >> 24) & 0x3F
        dst = (w[0] >> 1) & 0x3F
        if op != FENCBR and not (w[0] >> 30) & 1 and dst == 0:
            new = w[1] & ~(7 << 18)          # COND_FL = 0
            out = bytearray(blob)
            struct.pack_into(">I", out, ucode_off + (i + 1) * 4, fp_sources.unswap(new))
            return bytes(out), n
        i += 4 + (4 if block is not None else 0)
    raise SystemExit("FAIL: no R0-writing instruction to mutate")


def main(argv):
    if len(argv) not in (2, 3) or (len(argv) == 3 and argv[2] != "--control"):
        raise SystemExit(__doc__)
    blob = open(argv[1], "rb").read()
    try:
        if len(argv) == 3:
            mutant, index = mutate(blob)
            open(argv[1] + ".condfl", "wb").write(mutant)
            found = problems(mutant)
            # The clean container had no problems (the caller checked it first);
            # the mutant must be rejected for EXACTLY the mutation and nothing
            # else, or the control is satisfied by an unrelated reason.
            expected = "instruction %d has condition test 0 (not TR): a predicated write" % index
            if found != [expected]:
                sys.stderr.write("FAIL: the COND_FL control was not rejected for its own reason: "
                                 "expected [%s], got %r\n" % (expected, found))
                return 1
            print("control condfl rejected: " + expected)
            return 0
        found = problems(blob)
    except fp_sources.ContainerError as exc:
        sys.stderr.write("FAIL: unmodelled: %s\n" % exc)
        return 1
    if found:
        sys.stderr.write("FAIL: " + "; ".join(found) + "\n")
        return 1
    print("no predicated write, no condition-code write")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
