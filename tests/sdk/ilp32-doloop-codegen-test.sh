#!/usr/bin/env bash
# Codegen guard for GCC patch 0050: CTR is 64 bits wide and bdnz tests all of
# it, so a counted loop must not load CTR from a 32-bit value whose upper half
# was never cleared.  Under ILP32 the compiler used to move an SImode trip
# count straight into CTR; a count built from a zero-extended negative int
# carried bit 32 and the loop ran about 4G times (newlib printf "%f").
#
# Compiles tests/regression/ilp32-doloop/doloop.c at -O2 and -O3 in both data
# models and walks back from every mtctr to where its register was set: the
# chain must reach an instruction that leaves the upper half defined (a
# 32-bit rotate/shift, clrldi, a sign extension, li, or a 32-bit load with no
# offset added after it) before any full-width arithmetic (add, subf, ...).
# Controls (embedded below): the v0.19.0 candidate's listing of doloop.c and an
# lwz+addi count must be reported; the fixed compiler's clrldi+addi must not.
#
# Skips without PS3DEV (CI has no PPU compiler); run it in the release gate.
# usage: ilp32-doloop-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/scan.py" <<'EOF'
import re, sys

# Results whose full 64 bits are defined and at most 32 bits wide (or a
# sign-extended / 64-bit value): an offset added to them stays in range.
CLEAN = re.compile(r"^(clrldi|rlwinm\.?|rlwnm\.?|srwi\.?|slwi\.?|rotlwi|clrlwi\.?|"
                   r"extsw\.?|extsh\.?|extsb\.?|li|lis|lhz|lhzx|lbz|lbzx|"
                   r"lwa|lwax|ld|ldx|cntlzw\.?)$")
# A 32-bit load zero-extends: clean as a count by itself, but an offset added
# at full width to a negative int carries into bit 32.
LOAD32 = re.compile(r"^(lwz|lwzx|lwzu|lwzux)$")
# Instructions whose first operand is not a GPR they write.
NOWRITE = re.compile(r"^(cmp|fcmp|st|b|tw|td|dcb|mt|sync|isync|nop|lfs|lfd|stfiwx)")
LINE = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")

def scan(path):
    insns = []
    for raw in open(path):
        m = LINE.match(raw)
        if m:
            insns.append((m.group(1), [a.strip() for a in m.group(2).split("#")[0].split(",")]))
        elif re.match(r"^[0-9a-f]+ <", raw):
            insns.append(("<fn>", []))
    findings = 0
    for i, (op, args) in enumerate(insns):
        if op != "mtctr":
            continue
        reg, j, verdict, offset = args[0], i - 1, None, None
        while j >= 0 and verdict is None:
            o, a = insns[j]
            if o == "<fn>":
                verdict = "clean"  # an incoming argument: the caller extended it
            elif a and a[0] == reg and not NOWRITE.match(o):
                if CLEAN.match(o):
                    verdict = "clean"
                elif o == "rldicl" and len(a) == 4 and int(a[3], 0) >= 32:
                    verdict = "clean"
                elif LOAD32.match(o):
                    verdict = ("dirty: %s %s then %s" % (o, ",".join(a), offset)
                               if offset else "clean")
                elif o == "mr" and len(a) == 2:
                    reg = a[1]
                elif o == "addi" and len(a) == 3:
                    if int(a[2], 0) != 0 and offset is None:
                        offset = "addi %s" % ",".join(a)
                    reg = a[1]
                else:
                    verdict = "dirty: %s %s" % (o, ",".join(a))
            j -= 1
        if verdict and verdict != "clean":
            findings += 1
            print("finding: mtctr %s <- %s" % (args[0], verdict[7:]))
    print("%s: %d findings" % (path, findings))
    return findings

sys.exit(0 if scan(sys.argv[1]) == 0 else 3)
EOF

# The v0.19.0 candidate (c13ee74f) at -O2: CTR loaded from add r5,r9,r5 where
# r9 is a zero-extended lwz of a negative int.
cat > "$work/red.dis" <<'EOF'
0000000000000000 <.text.digits>:
   0:	fc 00 08 1e 	fctiwz  f0,f1
   4:	81 24 00 00 	lwz     r9,0(r4)
   8:	39 43 00 01 	addi    r10,r3,1
   c:	7c a9 2a 14 	add     r5,r9,r5
  10:	79 49 00 20 	clrldi  r9,r10,32
  14:	39 41 ff f4 	addi    r10,r1,-12
  18:	79 27 00 20 	clrldi  r7,r9,32
  1c:	2c 05 00 00 	cmpwi   r5,0
  20:	7c 00 57 ae 	stfiwx  f0,0,r10
  24:	81 41 ff f4 	lwz     r10,-12(r1)
  28:	39 4a 00 30 	addi    r10,r10,48
  2c:	99 43 00 00 	stb     r10,0(r3)
  30:	41 82 00 68 	beq     98 <.text.digits+0x98>
  34:	3d 42 00 00 	addis   r10,r2,0
  38:	7c e9 3b 78 	mr      r9,r7
  3c:	7c a9 03 a6 	mtctr   r5
EOF
# An offset added at full width to a zero-extended 32-bit load: *kp = -1 gives
# CTR 0x1_00000001.
cat > "$work/red-addi.dis" <<'EOF'
0000000000000000 <.text.f>:
   0:	81 24 00 00 	lwz     r9,0(r4)
   4:	38 a9 00 02 	addi    r5,r9,2
   8:	7c a9 03 a6 	mtctr   r5
EOF
# The fixed compiler's shape: the count is zero-extended, then biased by one.
cat > "$work/green.dis" <<'EOF'
0000000000000000 <.text.digits>:
  34:	39 45 ff ff 	addi    r10,r5,-1
  38:	3d 22 00 00 	addis   r9,r2,0
  3c:	79 4a 00 20 	clrldi  r10,r10,32
  40:	c1 89 00 00 	lfs     f12,0(r9)
  44:	7c e9 3b 78 	mr      r9,r7
  48:	39 4a 00 01 	addi    r10,r10,1
  4c:	7d 49 03 a6 	mtctr   r10
EOF
status=0
control() {  # <name> <expected exit> <expected text> <label>
    python3 "$work/scan.py" "$work/$1.dis" > "$work/$1.out"
    local rc=$?
    if [ $rc -eq "$2" ] && grep -q -- "$3" "$work/$1.out"; then
        echo "ilp32-doloop-codegen: ok   control: $4"
    else
        echo "ilp32-doloop-codegen: FAIL control: $4 (exit $rc)"
        cat "$work/$1.out"; status=1
    fi
}
control red 3 "finding: mtctr r5 <- add" "the v0.19.0 candidate listing is reported"
control red-addi 3 "finding: mtctr r5 <- lwz .* then addi" "an offset on a 32-bit load is reported"
control green 0 ": 0 findings" "a zero-extended count is accepted"

ps3dev="${PS3DEV:-}"
[ "${1:-}" = "--ps3dev" ] && ps3dev="$2"
if [ -z "$ps3dev" ]; then
    echo "ilp32-doloop-codegen: SKIP compiles (set PS3DEV or --ps3dev)"
    exit $status
fi
cc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
objdump="$ps3dev/ppu/bin/powerpc64-ps3-elf-objdump"
[ -x "$cc" ] || [ -x "$cc.exe" ] || { echo "ilp32-doloop-codegen: FAIL: no compiler at $cc"; exit 1; }
src="$root/tests/regression/ilp32-doloop/doloop.c"

for model in "" -mlp64; do
    for opt in -O2 -O3; do
        name="${model:-ilp32}$opt"
        if ! "$cc" ${EXTRA_CFLAGS:-} -mcpu=cell -mhard-float $model $opt -c "$src" -o "$work/$name.o"; then
            echo "ilp32-doloop-codegen: FAIL ${model:-ILP32} $opt: compile failed"; status=1; continue
        fi
        "$objdump" -d "$work/$name.o" > "$work/$name.dis"
        if ! grep -q mtctr "$work/$name.dis"; then
            echo "ilp32-doloop-codegen: FAIL ${model:-ILP32} $opt: no counted loop; the reproducer no longer reaches doloop"
            status=1
        elif python3 "$work/scan.py" "$work/$name.dis" > "$work/$name.out"; then
            echo "ilp32-doloop-codegen: ok   ${model:-ILP32} $opt: every CTR count has a defined upper half"
        else
            echo "ilp32-doloop-codegen: FAIL ${model:-ILP32} $opt"; cat "$work/$name.out"; status=1
        fi
    done
done

[ $status -eq 0 ] && echo "ilp32-doloop-codegen: PASS" || echo "ilp32-doloop-codegen: FAIL"
exit $status
