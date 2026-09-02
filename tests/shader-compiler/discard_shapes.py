"""Check the CF-2 discard shapes against what the reference emits.

Driven by discard-guard-test.sh.  Every expectation below was MEASURED by
compiling the same fixture with the reference compiler and disassembling
it, before the fixture's header was written; the design note carries the
shapes and the two anomalies.  Nothing here is derived from our own
lowering, which is the mistake that made three earlier assertions stronger
than the oracle.

Argument form:  discard_shapes.py <case> <dump>
"""

import sys

from ucode_decode import decode

MOV, MUL, ADD = 0x01, 0x02, 0x03
SLT, SGE, SLE, SGT, SNE, SEQ = 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
MAX = 0x09
KIL = 0x12
FX12, FP32 = 2, 0

COMPARISONS = {SLT, SGE, SLE, SGT, SNE, SEQ}
# `&&` is a multiply and `||` is a saturating add for the reference; ours
# spells `||` as MAX, which is equal on the 0/1 booleans a comparison
# produces and is its own byte-identity item.
COMBINERS = {MUL, ADD, MAX}


def fail(msg):
    raise SystemExit("FAIL: " + msg)


def load(path):
    return [d for d in decode(path) if d["opcode"] != 0 or d["ccw"]]


def kills(insns):
    return [i for i, d in enumerate(insns) if d["opcode"] == KIL]


def guard_of(insns, kill_pos):
    """The instruction that set the condition register for this kill.

    Searching backwards for the nearest cc_update rather than assuming
    adjacency: the scheduler is free to put an unrelated instruction
    between them, and an assertion that forbids that would be stronger
    than the hardware.
    """
    for i in range(kill_pos - 1, -1, -1):
        if insns[i]["ccw"]:
            return insns[i]
    return None


def one_kill(insns, case):
    ks = kills(insns)
    if len(ks) != 1:
        fail("%s: expected exactly one KIL, decoded %d" % (case, len(ks)))
    g = guard_of(insns, ks[0])
    if g is None:
        fail("%s: the KIL has no condition-register write before it - "
             "nothing sets what it tests" % case)
    if not g["none"]:
        fail("%s: the guard writes a general register (dst=%d); the "
             "reference writes the condition register only" % (case, g["dst"]))
    return insns[ks[0]], g


def expect(case, got, want, what):
    if got != want:
        fail("%s: %s is %s, the reference emits %s" % (case, what, got, want))


def check(case, path):
    insns = load(path)
    if not insns:
        fail("%s: decoded no instructions" % case)

    if case in ("lt", "ge", "ge_rev", "not", "uncond", "and", "or",
                "nested", "merge", "else_arm"):
        kil, g = one_kill(insns, case)

    if case == "lt":
        expect(case, "0x%02X" % g["opcode"], "0x%02X" % SLT, "the guard opcode")
        expect(case, g["prec"], FX12, "the guard precision")
        expect(case, kil["cond"], 5, "the KIL test (5 is NE)")
    elif case == "ge":
        # The measured anomaly: a lone `>=` feeding a kill stays fp32
        # where every other comparison demotes to fx12.  Pinned so a
        # later tidy-up of the precision table cannot normalise it away
        # with every other test still green.
        expect(case, "0x%02X" % g["opcode"], "0x%02X" % SGE, "the guard opcode")
        expect(case, g["prec"], FP32, "the guard precision")
        expect(case, kil["cond"], 5, "the KIL test (5 is NE)")
    elif case == "ge_rev":
        # The same predicate spelled `0.5 <= a`: the reference keeps the
        # SOURCE operator, so this one IS demoted.  The pair is the proof
        # that the anomaly belongs to the opcode and not to the operands.
        expect(case, "0x%02X" % g["opcode"], "0x%02X" % SLE, "the guard opcode")
        expect(case, g["prec"], FX12, "the guard precision")
    elif case == "not":
        # A negated guard flips the KIL's TEST and leaves the comparison
        # alone.  Inverting the comparison instead would give the same
        # pixels here and a different answer on NaN, and a different
        # container either way.
        expect(case, kil["cond"], 2, "the KIL test (2 is EQ)")
        if not any(d["opcode"] == SGT for d in insns):
            fail("not: the comparison was inverted - no SGT in the ucode. "
                 "The reference keeps `>` and flips the kill's test.")
    elif case == "uncond":
        # Not a special encoding: a true condition is materialised and
        # the same KIL used.
        expect(case, "0x%02X" % g["opcode"], "0x%02X" % MOV, "the guard opcode")
        expect(case, g["prec"], FX12, "the guard precision")
        expect(case, kil["cond"], 5, "the KIL test (5 is NE)")
    elif case in ("and", "or", "nested"):
        if g["opcode"] not in COMBINERS:
            fail("%s: the instruction feeding the kill is 0x%02X, not a "
                 "logical combiner - a compound guard is a chain whose "
                 "LAST link carries the cc_update" % (case, g["opcode"]))
        expect(case, g["prec"], FX12, "the combiner's precision")
        expect(case, kil["cond"], 5, "the KIL test (5 is NE)")
        cmps = [d for d in insns if d["opcode"] in COMPARISONS]
        if len(cmps) < 2:
            fail("%s: decoded %d comparisons; a compound guard needs both "
                 "of its terms" % (case, len(cmps)))
    elif case == "merge":
        # The whole reason the guard is computed by intersection: the two
        # arms of the merge above this discard CANCEL, so the guard is the
        # inner comparison ALONE.  A conjunction here would mean the
        # then-arm literal was kept.
        if g["opcode"] not in COMPARISONS:
            fail("merge: the kill's guard is 0x%02X, not a comparison - the "
                 "arms of the merge did not cancel and the guard carries a "
                 "conjunction it should not have" % g["opcode"])
        ccws = [d for d in insns if d["ccw"]]
        if len(ccws) != 2:
            fail("merge: %d instructions write the condition register; the "
                 "shape has exactly two - the merge's select and the kill's "
                 "guard" % len(ccws))
    elif case == "ops":
        ks = kills(insns)
        if len(ks) != 6:
            fail("ops: expected six KILs, one per discard statement, "
                 "decoded %d" % len(ks))
        seen = {}
        for k in ks:
            g = guard_of(insns, k)
            if g is None:
                fail("ops: a KIL has no condition-register write before it")
            seen[g["opcode"]] = g["prec"]
        want = {SLT, SGT, SLE, SGE, SEQ, SNE}
        if set(seen) != want:
            fail("ops: the guard opcodes are %s; the six source operators "
                 "must each keep their own comparison, in source operand "
                 "order (the reference does not canonicalise)"
                 % sorted("0x%02X" % o for o in seen))
        for opcode, prec in seen.items():
            want_prec = FP32 if opcode == SGE else FX12
            if prec != want_prec:
                fail("ops: guard 0x%02X has precision %d, the reference "
                     "emits %d (SGE is the one comparison it does not "
                     "demote to fx12)" % (opcode, prec, want_prec))
    elif case == "else_arm":
        # The kill is reached through `else`, so it fires where the branch
        # condition is FALSE: the comparison is the one the source wrote
        # and the KIL's TEST carries the polarity.  A lowering that
        # inverted the comparison instead would kill the same fragments
        # here and a different set on a NaN, and would be a different
        # container either way.  This is the shape the DEFAULT path gets
        # backwards (t_79fc6bf7) - it emits the same SGT with the test on
        # NE and kills exactly the fragments that must survive.
        expect(case, "0x%02X" % g["opcode"], "0x%02X" % SGT,
               "the guard opcode")
        expect(case, kil["cond"], 2, "the KIL test (2 is EQ)")
        if not any(d["dst"] == 0 and d["mask"] and not d["none"]
                   for d in insns):
            fail("else_arm: nothing writes the colour output.  The store "
                 "is on the arm the branch takes, and it is only safe to "
                 "run it unconditionally because the other arm kills - "
                 "dropping it would make that argument vacuous.")
    elif case == "two":
        # Two discards, two kills, and the stores between them survive.
        # The stores sit in blocks the control flow cannot skip - each is
        # on every path to the exit - which is what lets the flattened
        # program run all three in order and keep the last.
        ks = kills(insns)
        if len(ks) != 2:
            fail("two: expected two KILs, one per discard statement, "
                 "decoded %d" % len(ks))
        for k in ks:
            g = guard_of(insns, k)
            if g is None:
                fail("two: a KIL has no condition-register write before it")
            if not g["none"]:
                fail("two: a kill's guard writes a general register "
                     "(dst=%d) instead of the condition register" % g["dst"])
        if ks[1] == len(insns) - 1 and ks[0] == len(insns) - 2:
            fail("two: both kills are the last two instructions, so the "
                 "work between and after them was dropped - the stores are "
                 "the point of this fixture")
    elif case == "then_work":
        ks = kills(insns)
        if len(ks) != 1:
            fail("then_work: expected one KIL, decoded %d" % len(ks))
        if ks[0] == len(insns) - 1:
            fail("then_work: the KIL is the last instruction.  A kill is "
                 "NOT a terminator - the work after a discard runs, which "
                 "is what th06_notex proved (t_72810bd7).")
        if not any(d["end"] for d in insns[ks[0] + 1:]):
            fail("then_work: nothing after the KIL carries the "
                 "program-end flag; the post-discard work was dropped")
    else:
        fail("unknown case '%s'" % case)


def main():
    check(sys.argv[1], sys.argv[2])
    print("discard shape ok: %s" % sys.argv[1])


if __name__ == "__main__":
    main()
