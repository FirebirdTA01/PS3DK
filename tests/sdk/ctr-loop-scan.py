#!/usr/bin/env python3
"""Loop-aware CTR scan (t_54ed85f8).

Reads objdump -d output and judges every mtctr as a possible loop count
(unproven = conservative) unless a TERMINAL is reached before any branch.
Terminals (the CTR value is provably not loop-count):

  * another mtctr          - CTR redefined, original value never consumed
  * bctr / bctrl           - CTR consumed as an indirect jump/call target
  * bl / blr               - unconditional call / return, CTR value is dead
  * b to another function  - tail call (target "<sym>" with no "+off"),
                             CTR is call-clobbered

Everything else - any other branch (bdnz, bdz, be, bne, bgt, ..., a plain b
to a local label, the common rotated-loop shape), mfctr (it only READS CTR,
does not consume or redefine it), and end of the function or listing (no
straight-line proof of non-use) - keeps the mtctr a possible loop count.

For a loop-consumed mtctr it walks back to where the register was set:
a full-width definition (clrldi, rldicl mb>=32, rlw*, ext*, li, ...) is
clean; a 32-bit load is clean only if no offset was added after it; anything
else (add, subf, ...) is a finding.

usage: ctr-loop-scan.py <objdump.txt> [--self-test]
exit:  0 no findings, 3 findings, 2 self-test failure
"""
import re, sys

CLEAN = re.compile(r"^(clrldi|rlwinm\.?|rlwnm\.?|srwi\.?|slwi\.?|rotlwi|clrlwi\.?|"
                   r"extsw\.?|extsh\.?|extsb\.?|li|lis|lhz|lhzx|lbz|lbzx|"
                   r"lwa|lwax|ld|ldx|cntlzw\.?)$")
LOAD32 = re.compile(r"^(lwz|lwzx|lwzu|lwzux)$")
NOWRITE = re.compile(r"^(cmp|fcmp|st|tw|td|dcb|mt|sync|isync|nop|lfs|lfd|stfiwx)")
LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")
FN = re.compile(r"^([0-9a-f]+) <([^>]*)>:")

def parse(lines, funcs=None):
    # funcs: the FUNC symbols of the object (objdump -t), or None.  objdump prints every symbol as a
    # "<sym>:" label, local labels included, so with a FUNC list only a function symbol starts a new
    # function; without one every label does (and a b/bl target is never proof of another function).
    insns, fn = [], "?"
    for raw in lines:
        m = FN.match(raw)
        if m:
            if funcs is not None and m.group(2) not in funcs:
                continue
            fn = m.group(2)
            insns.append(("<fn>", [], fn, m.group(1)))
            continue
        m = LINE.match(raw)
        if m:
            args = [a.strip() for a in m.group(3).split("#")[0].split(",")]
            insns.append((m.group(2), args, fn, m.group(1)))
    return insns

def origin(insns, i, reg):
    j, offset = i - 1, None
    while j >= 0:
        o, a, _, _ = insns[j]
        if o == "<fn>":
            return "clean"
        if a and a[0] == reg and not NOWRITE.match(o):
            if CLEAN.match(o):
                return "clean"
            if o == "rldicl" and len(a) == 4 and int(a[3], 0) >= 32:
                return "clean"
            if LOAD32.match(o):
                return "dirty: %s %s then %s" % (o, ",".join(a), offset) if offset else "clean"
            if o == "mr" and len(a) == 2:
                reg = a[1]
            elif o == "addi" and len(a) == 3:
                if int(a[2], 0) != 0 and offset is None:
                    offset = "addi " + ",".join(a)
                reg = a[1]
            else:
                return "dirty: %s %s" % (o, ",".join(a))
        j -= 1
    return "clean"

def call_target(a, fn, funcs):
    """The symbol a b/bl reaches when it is PROVEN another function: a FUNC symbol (objdump -t) other than
    the current one, with no "+off".  Anything else - a local label, this function, no symbol, no FUNC
    list - is None: the branch may stay inside the function and reach a counted loop (codex 15e26177)."""
    if funcs is None:
        return None
    m = re.search(r"<([^>]+)>", a[-1]) if a else None
    sym = m.group(1) if m else None
    if sym and "+" not in sym and sym != fn and sym in funcs:
        return sym
    return None

def scan(lines, out=sys.stdout, funcs=None):
    insns = parse(lines, funcs)
    findings, loops, other = 0, 0, []
    for i, (op, args, fn, addr) in enumerate(insns):
        if op != "mtctr":
            continue
        # A mtctr is a NON-LOOP use only if a real TERMINAL is reached before
        # any branch.  Terminals (CTR provably not a loop count):
        #   another mtctr          - CTR redefined
        #   bctr / bctrl           - CTR consumed as an indirect target
        #   bl / blr               - call / return, CTR value dead
        #   b to another function  - tail call: a plain "b" whose last arg is
        #                             another symbol with no "+off" (call
        #                             clobbers CTR).  A plain b to a local
        #                             label (<.f+0xN> or <.text...>) is the
        #                             COMMON rotated-loop shape, so it is NOT
        #                             a terminal.
        #
        # NOT terminals (the mtctr stays a possible loop count): any branch
        # (bdnz, bdz, be, bne, bgt, ble, ...), mfctr (it only READS CTR - a
        # later bdnz still sees the count), and simply reaching the end of
        # the function / listing (no straight-line proof of non-use).
        terminal = None
        for k in range(i + 1, len(insns)):
            o, a = insns[k][0], insns[k][1]
            if o == "<fn>":
                break                       # end of function/listing: proves nothing
            if o == "mtctr":
                terminal = "mtctr"
                break
            if o in ("bctr", "bctrl"):
                terminal = o
                break
            if o == "blr":
                terminal = o                       # return: CTR is dead
                break
            if o == "bl":
                # A call clobbers CTR (volatile) - but only a call to a PROVEN other function.
                # A bl to a local label is a branch-and-link that can reach the loop.
                terminal = "bl(call %s)" % call_target(a, fn, funcs) if call_target(a, fn, funcs) else None
                break
            if o == "b":
                # Real objdump prints a branch target as "<addr> <sym>" (the
                # address is separate from the operand).  Real form: "b  0
                # <.f>", "b  1c <.g>".  A local label carries a "+off"
                # (b  14 <.f+0x14>).  Tail call iff <sym> exists, has no
                # "+off", and is not this function (a call clobbers CTR);
                # anything else (local label, own entry, no <sym>) stays a
                # loop use.
                if call_target(a, fn, funcs):
                    terminal = "b(tailcall %s)" % call_target(a, fn, funcs)   # CTR call-clobbered
                    break
                terminal = None                    # local / unproven b = loop use
                break
            if o.startswith("b"):
                terminal = None                    # any other branch = loop use
                break
            # mfctr / plain instructions: no CTR effect -> keep walking
        if terminal is not None:
            other.append("%s %s mtctr %s -> %s" % (fn, addr, args[0], terminal))
            continue
        loops += 1
        v = origin(insns, i, args[0])
        if v != "clean":
            findings += 1
            print("finding: %s %s mtctr %s <- %s" % (fn, addr, args[0], v[7:]), file=out)
    for o in other:
        print("non-loop CTR use (not a hazard): " + o, file=out)
    print("judged mtctr (possible loop count): %d, findings: %d, non-loop mtctr: %d" % (loops, findings, len(other)), file=out)
    return findings

def self_test():
    import io
    cases = {
        "red-add": ("""0000000000000000 <.f>:
   0:	81 24 00 00 	lwz     r9,0(r4)
   4:	7c a9 2a 14 	add     r5,r9,r5
   8:	7c a9 03 a6 	mtctr   r5
   c:	42 00 ff fc 	bdnz    8 <.f+0x8>
""", 1),
        "red-load-addi": ("""0000000000000000 <.f>:
   0:	81 24 00 00 	lwz     r9,0(r4)
   4:	38 a9 00 02 	addi    r5,r9,2
   8:	7c a9 03 a6 	mtctr   r5
   c:	42 00 ff fc 	bdnz    8 <.f+0x8>
""", 1),
        "green-extended": ("""0000000000000000 <.f>:
   0:	79 4a 00 20 	clrldi  r10,r10,32
   4:	39 4a 00 01 	addi    r10,r10,1
   8:	7d 49 03 a6 	mtctr   r10
   c:	42 00 ff fc 	bdnz    8 <.f+0x8>
""", 0),
        # EMP spectralWater shape: CTR as a scratch register, no counted loop.
        # The real terminal is the blr at the end (the fall-off-end does NOT
        # prove non-use; it only survives because the mfctr is followed by a
        # terminal).
        "green-scratch": ("""0000000000000000 <.f>:
    0:	7c 00 00 26 	mfocrf  r0,128
    4:	7c 00 03 78 	or      r0,r0,r9
    8:	7c 09 03 a6 	mtctr   r0
    c:	60 00 00 00 	nop
   10:	7c 09 02 a6 	mfctr   r0
   14:	4e 80 00 20 	blr
""", 0),
        # Two paths into one loop: the first mtctr reaches the bdnz through
        # the ble; a forward linear scan used to stop at the second mtctr.
        "red-branchy": ("""0000000000000000 <.f>:
   0:	81 24 00 00 	lwz     r9,0(r4)
   4:	7d 49 2a 14 	add     r10,r9,r5
   8:	7d 49 03 a6 	mtctr   r10
   c:	40 81 00 10 	ble     1c <.f+0x1c>
  10:	39 4a ff ff 	addi    r10,r10,-1
  14:	7d 49 03 a6 	mtctr   r10
  18:	60 00 00 00 	nop
  1c:	42 00 ff fc 	bdnz    18 <.f+0x18>
""", 2),
        "green-indirect-call": ("""0000000000000000 <.f>:
    0:	81 29 00 00 	lwz     r9,0(r9)
    4:	7d 29 03 a6 	mtctr   r9
    8:	4e 80 04 21 	bctrl
""", 0),
        # Codex P1 (t_54ed85f8 review): mfctr only READS CTR; it does not
        # consume or redefine it, so a later bdnz still sees the original
        # count and the mtctr IS a loop use.  Old version (mfctr treated as
        # non-loop terminal) reported 0 findings here; it must report 1.
        "red-read-then-loop": ("""0000000000000000 <.f>:
    0:	81 24 00 00 	lwz     r9,0(r4)
    4:	38 a9 00 02 	addi    r5,r9,2
    8:	7c a9 03 a6 	mtctr   r5
    c:	7c 0a 02 a6 	mfctr   r10
   10:	42 00 ff fc 	bdnz    10 <.f+0x10>
""", 1),
        # codex P1-b (v2 review): a plain b to a LOCAL label is the common
        # rotated-loop shape, not proof the count is unused.  b .f+0x14 must
        # NOT be a terminal; the mtctr stays a possible loop count (finding).
        "red-b-into-loop": ("""0000000000000000 <.f>:
    0:	81 24 00 00 	lwz     r9,0(r4)
    4:	38 a9 00 02 	addi    r5,r9,2
    8:	7c a9 03 a6 	mtctr   r5
    c:	48 00 00 08 	b       14 <.f+0x14>
   10:	60 00 00 00 	nop
   14:	42 00 ff fc 	bdnz    10 <.f+0x10>
   18:	4e 80 00 20 	blr
""", 1),
        # codex P1-a (v2 review): a listing that ENDS at the mtctr proves
        # nothing; end-of-function/listing is not a non-loop terminal, so the
        # count is still judged (finding).  Guards the two embedded .sh RED
        # controls that fall off the end.
        "red-truncated": ("""0000000000000000 <.text.f>:
    0:	81 24 00 00 	lwz     r9,0(r4)
    4:	38 a9 00 02 	addi    r5,r9,2
    8:	7c a9 03 a6 	mtctr   r5
""", 1),
        # codex P1-b (v2 review): a b to ANOTHER function (target without
        # "+off") is a tail call, which clobbers CTR - a genuine non-loop
        # terminal.  Must stay green.  Written in real objdump form:
        # "b  <addr> <sym>" (the address is separate from the symbol).
        "green-b-tailcall": ("""0000000000000000 <.f>:
    0:	81 24 00 00 	lwz     r9,0(r4)
    4:	38 a9 00 02 	addi    r5,r9,2
    8:	7c a9 03 a6 	mtctr   r5
    c:	48 00 00 10 	b       1c <.g>
""", 0, {".f", ".g"}),
        # P2 (v3/v4 review): a b to the CURRENT function's entry is a local
        # branch, not a call - "b <addr> <.f>" must match fn ".f" (the symbol
        # is extracted from <...>, the address <addr> is ignored), so it is
        # NOT a tail-call terminal.  Real objdump form, not "b <.f>".
        "red-b-self": ("""0000000000000000 <.f>:
    0:	81 24 00 00 	lwz     r9,0(r4)
    4:	38 a9 00 02 	addi    r5,r9,2
    8:	7c a9 03 a6 	mtctr   r5
    c:	48 00 00 00 	b       0 <.f>
""", 1),
        # codex 15e26177: a "<sym>:" label in objdump output is not proof of a function.  A b to a local
        # label printed as its own symbol (hand-written assembly: loop_entry) reaches the counted loop.
        # With the object's FUNC list (only f) the label is not a boundary; without one the target is
        # unproven.  Either way the count is judged (finding).
        "red-b-local-label": ('0000000000000000 <f>:\n    0:\t81 24 00 00 \tlwz     r9,0(r4)\n    4:\t38 a9 00 02 \taddi    r5,r9,2\n    8:\t7c a9 03 a6 \tmtctr   r5\n    c:\t48 00 00 08 \tb       14 <loop_entry>\n   10:\t60 00 00 00 \tnop\n\n0000000000000014 <loop_entry>:\n   14:\t60 00 00 00 \tnop\n   18:\t42 00 ff fc \tbdnz    14 <loop_entry>\n   1c:\t4e 80 00 20 \tblr\n', 1, {"f"}),
        "red-b-local-label-no-funcs": ('0000000000000000 <f>:\n    0:\t81 24 00 00 \tlwz     r9,0(r4)\n    4:\t38 a9 00 02 \taddi    r5,r9,2\n    8:\t7c a9 03 a6 \tmtctr   r5\n    c:\t48 00 00 08 \tb       14 <loop_entry>\n   10:\t60 00 00 00 \tnop\n\n0000000000000014 <loop_entry>:\n   14:\t60 00 00 00 \tnop\n   18:\t42 00 ff fc \tbdnz    14 <loop_entry>\n   1c:\t4e 80 00 20 \tblr\n', 1, None),
        # codex 15e26177: bl does not itself clobber CTR; a bl to a local label reaches the loop too.
        "red-bl-local-label": ('0000000000000000 <f>:\n    0:\t81 24 00 00 \tlwz     r9,0(r4)\n    4:\t38 a9 00 02 \taddi    r5,r9,2\n    8:\t7c a9 03 a6 \tmtctr   r5\n    c:\t48 00 00 09 \tbl      14 <loop_entry>\n   10:\t60 00 00 00 \tnop\n\n0000000000000014 <loop_entry>:\n   14:\t60 00 00 00 \tnop\n   18:\t42 00 ff fc \tbdnz    14 <loop_entry>\n   1c:\t4e 80 00 20 \tblr\n', 1, {"f"}),
        # A b to another function is a tail call only with proof: no FUNC list -> judged.
        "red-b-tailcall-unproven": ('0000000000000000 <.f>:\n    0:\t81 24 00 00 \tlwz     r9,0(r4)\n    4:\t38 a9 00 02 \taddi    r5,r9,2\n    8:\t7c a9 03 a6 \tmtctr   r5\n    c:\t48 00 00 10 \tb       1c <.g>\n', 1, None),
        # A bl to a proven other function is a call (CTR is volatile across it): not a count.
        "green-bl-call": ('0000000000000000 <.f>:\n    0:\t81 24 00 00 \tlwz     r9,0(r4)\n    4:\t38 a9 00 02 \taddi    r5,r9,2\n    8:\t7c a9 03 a6 \tmtctr   r5\n    c:\t48 00 00 11 \tbl      1c <.g>\n', 0, {".f", ".g"}),
    }
    ok = True
    for name, case in cases.items():
        text, want, funcs = case if len(case) == 3 else (case[0], case[1], None)
        got = scan(text.splitlines(), out=io.StringIO(), funcs=funcs)
        print("self-test %-20s %s (findings %d, want %d)" % (name, "ok" if got == want else "FAIL", got, want))
        ok &= got == want
    return ok

if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(0 if self_test() else 2)
    funcs = None
    if "--funcs" in sys.argv:
        funcs = set(l.strip() for l in open(sys.argv[sys.argv.index("--funcs") + 1]) if l.strip())
    sys.exit(0 if scan(open(sys.argv[1], errors="replace"), funcs=funcs) == 0 else 3)
