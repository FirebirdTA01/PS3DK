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
#
# The scanner itself now lives in the repo at tests/sdk/ctr-loop-scan.py and
# is loop-aware: a mtctr is judged as a possible loop count (unproven, so
# conservative) unless a TERMINAL is reached first.  Terminals (CTR provably
# not a count): another mtctr (CTR redefined), bctr/bctrl (indirect
# call/jump), bl/blr (call/return, value dead), or a plain b to ANOTHER
# function (tail call - clobbers CTR).  NOT terminals: any other branch
# (bdnz, bdz, be, bne, bgt, ...), mfctr (it only READS CTR, so a later bdnz
# still sees the count), and the end of the function/listing (no straight-
# line proof of non-use).  So a count that drives a bdnz - including the
# read-then-loop shape (mfctr before the bdnz) or the common rotated
# b-into-local-label loop - is a finding.  Non-loop uses are reported
# separately and never counted.
#
# Controls (embedded below): the v0.19.0 candidate's listing of doloop.c and
# an lwz+addi count must be reported; the fixed compiler's clrldi+addi must
# not.  The scanner's own 10-case self-test (red-add, red-load-addi,
# green-extended, green-scratch, red-branchy, green-indirect-call,
# red-read-then-loop, red-b-into-loop, red-truncated, green-b-tailcall)
# runs first, without a compiler, so the guard fails closed if the logic
# regresses even on a build host with no PPU toolchain.
#
# Skips the compiles without PS3DEV (CI has no PPU compiler); the self-test
# and the controls still run and are the fast CI-facing coverage.
# usage: ilp32-doloop-codegen-test.sh [--ps3dev DIR]
#   EXTRA_CFLAGS is added to every compile (e.g. -B<dir> to test a candidate cc1).
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
scanner="$root/tests/sdk/ctr-loop-scan.py"
[ -f "$scanner" ] || { echo "ilp32-doloop-codegen: FAIL: missing $scanner"; exit 1; }

# First run the scanner's own self-test: 10 synthetic cases covering the
# loop-usage rules (which instructions make a count clean/dirty), the
# non-loop terminals (mfocrf-scratch with a blr, bctrl indirect call, b
# tail-call), and the loop-use cases (read-then-loop via mfctr, a plain b
# into a local label, and a listing truncated at the mtctr).  The exit code
# is 0 only if all ten pass (2 otherwise), so pass/fail is authoritative;
# the per-case line counts below make the "10/10" requirement explicit.
python3 "$scanner" --self-test > "$work/self-test.out" 2>&1
rctest=$?
n_ok=$(grep -c ' ok (findings' "$work/self-test.out")
n_fail=$(grep -c 'FAIL (findings' "$work/self-test.out")
if [ $rctest -ne 0 ] || [ "$n_ok" -ne 10 ] || [ "$n_fail" -ne 0 ]; then
    echo "ilp32-doloop-codegen: FAIL self-test (exit $rctest, ${n_ok}/10 ok, ${n_fail} fail)"
    cat "$work/self-test.out"
    exit 1
fi
echo "ilp32-doloop-codegen: ok   self-test: 10/10 synthetic cases"

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
control() {  # <name> <expected exit> <expected regex> <label>
    python3 "$scanner" "$work/$1.dis" > "$work/$1.out"
    local rc=$?
    if [ $rc -eq "$2" ] && grep -qE -- "$3" "$work/$1.out"; then
        echo "ilp32-doloop-codegen: ok   control: $4"
    else
        echo "ilp32-doloop-codegen: FAIL control: $4 (exit $rc)"
        cat "$work/$1.out"; status=1
    fi
}
# Scanner output (tests/sdk/ctr-loop-scan.py) for the three embedded listings:
#   finding: <fn> <addr> mtctr <reg> <- <verdict>
#   judged mtctr (possible loop count): N, findings: M, non-loop mtctr: K
control red       3  'finding: .* mtctr r5 <- add .*'              "the v0.19.0 candidate listing is reported"
control red-addi  3  'finding: .* mtctr r5 <- lwz .* then addi .*' "an offset on a 32-bit load is reported"
control green     0  'findings: 0'                                 "a zero-extended count is accepted"

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
        elif python3 "$scanner" "$work/$name.dis" > "$work/$name.out"; then
            echo "ilp32-doloop-codegen: ok   ${model:-ILP32} $opt: every CTR count has a defined upper half"
        else
            echo "ilp32-doloop-codegen: FAIL ${model:-ILP32} $opt"; cat "$work/$name.out"; status=1
        fi
    done
done

[ $status -eq 0 ] && echo "ilp32-doloop-codegen: PASS" || echo "ilp32-doloop-codegen: FAIL"
exit $status
