#!/usr/bin/env python3
"""Loop-aware CTR scan (draft for t_54ed85f8; not yet in the repo).

Reads objdump -d output and judges every mtctr as a possible loop count unless
straight-line code (no branch in between) PROVES another use: an mfctr (CTR as
a scratch register) or a bctr/bctrl (indirect jump or call).  Those proven
uses are listed separately and never counted as loop hazards.

For a loop-consumed mtctr it walks back from the instruction to where the
register was set, with the rules of tests/sdk/ilp32-doloop-codegen-test.sh:
a full-width defined value (clrldi, rldicl mb>=32, rlw*, ext*, li, ...) is
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
NOWRITE = re.compile(r"^(cmp|fcmp|st|b|tw|td|dcb|mt|sync|isync|nop|lfs|lfd|stfiwx)")
LOOPBR = re.compile(r"^bd(nz|z)")                      # bdnz, bdz, bdnzt, bdzf, bdnzl, ...
END = re.compile(r"^(mtctr|mfctr|bl|bla|bctrl|bctr)$")  # CTR redefined, read, or a call
LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")
FN = re.compile(r"^([0-9a-f]+) <([^>]*)>:")

def parse(lines):
    insns, fn = [], "?"
    for raw in lines:
        m = FN.match(raw)
        if m:
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

def scan(lines, out=sys.stdout):
    insns = parse(lines)
    findings, loops, other = 0, 0, []
    for i, (op, args, fn, addr) in enumerate(insns):
        if op != "mtctr":
            continue
        # Non-loop only when PROVEN: straight-line code (no branch of any
        # kind in between) reaches an mfctr (CTR as a scratch register) or a
        # bctr/bctrl (indirect jump or call).  Anything else - a bdnz, a
        # conditional branch that may lead to one, another mtctr reached on
        # one path only - is judged as a possible loop count.
        use = None
        for k in range(i + 1, len(insns)):
            o = insns[k][0]
            if o == "<fn>":
                break
            if o in ("mfctr", "bctr", "bctrl"):
                use = o
                break
            if o.startswith("b") or o == "mtctr":
                break
        if use is not None:
            other.append("%s %s mtctr %s -> %s" % (fn, addr, args[0], use))
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
        "green-scratch": ("""0000000000000000 <.f>:
   0:	7c 00 00 26 	mfocrf  r0,128
   4:	7c 00 03 78 	or      r0,r0,r9
   8:	7c 09 03 a6 	mtctr   r0
   c:	60 00 00 00 	nop
  10:	7c 09 02 a6 	mfctr   r0
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
    }
    ok = True
    for name, (text, want) in cases.items():
        got = scan(text.splitlines(), out=io.StringIO())
        print("self-test %-20s %s (findings %d, want %d)" % (name, "ok" if got == want else "FAIL", got, want))
        ok &= got == want
    return ok

if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(0 if self_test() else 2)
    sys.exit(0 if scan(open(sys.argv[1], errors="replace")) == 0 else 3)
