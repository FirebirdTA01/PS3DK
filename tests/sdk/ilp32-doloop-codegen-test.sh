#!/usr/bin/env bash
# Codegen guard for the counted-loop fix of GCC patch 0050, carried by 0059's
# DImode Pmode since 0050 was retired: CTR is 64 bits wide and bdnz tests all of
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
# call/jump), blr (return), or a b/bl whose target is PROVEN another function
# (a FUNC symbol from objdump -t other than the current one: tail call or call,
# CTR is volatile).  An objdump "<sym>:" label alone is not that proof: a b or
# bl to a local label can reach the counted loop, so it is judged.  NOT terminals: any other branch
# (bdnz, bdz, be, bne, bgt, ...), mfctr (it only READS CTR, so a later bdnz
# still sees the count), and the end of the function/listing (no straight-
# line proof of non-use).  So a count that drives a bdnz - including the
# read-then-loop shape (mfctr before the bdnz) or the common rotated
# b-into-local-label loop - is a finding.  Non-loop uses are reported
# separately and never counted.
#
# Controls (embedded below): the v0.19.0 candidate's listing of doloop.c and
# an lwz+addi count must be reported; the fixed compiler's clrldi+addi must
# not.  Each compiled object must show at least one WITNESSED counted loop (a
# bd* branch reachable from a judged mtctr, with CTR intact, whose in-function
# target leads back to it - a cycle; unresolved shapes fail closed), and
# objdump's exit status counts (judge's own controls: bctrl-only, conditional-
# only and truncated listings, and an objdump that prints then exits 1, fail).  The scanner's own 16-case
# self-test (red-add, red-load-addi, green-extended, green-scratch,
# red-branchy, green-indirect-call, red-read-then-loop, red-b-into-loop,
# red-truncated, green-b-tailcall, red-b-self, red-b-local-label,
# red-b-local-label-no-funcs, red-bl-local-label, red-b-tailcall-unproven,
# green-bl-call) runs first, without a compiler, so the guard fails closed if
# the logic regresses even on a build host with no PPU toolchain.
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

# First run the scanner's own self-test: 16 synthetic cases covering the
# loop-usage rules (which instructions make a count clean/dirty), the
# non-loop terminals (mfocrf-scratch with a blr, bctrl indirect call, a b or
# bl to a proven other function), and the loop-use cases (read-then-loop via
# mfctr, a plain b into a local label, a listing truncated at the mtctr, a b to
# the function's own entry, a b or bl to a local label printed as a symbol,
# an unproven tail call).  The exit code is 0 only if all twenty-one pass (2
# otherwise), so pass/fail is authoritative; the per-case line counts below
# make the "21/21" requirement explicit (16 hazard cases + 5 loop-witness
# cases: normal and rotated loops witnessed; a forward-only bdnz, a backward
# bdnz whose loop body calls, and a bdnz after a return not witnessed).
python3 "$scanner" --self-test > "$work/self-test.out" 2>&1
rctest=$?
n_ok=$(grep -cE ' ok \((findings|witness)' "$work/self-test.out")
n_fail=$(grep -cE 'FAIL \((findings|witness)' "$work/self-test.out")
if [ $rctest -ne 0 ] || [ "$n_ok" -ne 21 ] || [ "$n_fail" -ne 0 ]; then
    echo "ilp32-doloop-codegen: FAIL self-test (exit $rctest, ${n_ok}/21 ok, ${n_fail} fail)"
    cat "$work/self-test.out"
    exit 1
fi
echo "ilp32-doloop-codegen: ok   self-test: 21/21 synthetic cases"

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

# judge <objdump> <object> <name>: the object's listing (objdump -d) and its FUNC symbols (objdump -t), both
# status-checked, then the scanner with the FUNC list.  Passes only if the scan has no finding AND witnessed at least
# one CTR-controlled loop (a bd* cycle reachable from a judged mtctr): a listing whose mtctr feed no loop branch
# tests nothing (codex 15e26177, 67047a74).  Possible counts are still all judged for hazards.  The reason
# for a failure is left in $work/<name>.why.
judge() {
    local od=$1 obj=$2 name=$3 rc witnessed
    if ! "$od" -d "$obj" > "$work/$name.dis" 2> "$work/$name.err"; then
        echo "objdump -d failed: $(head -n 1 "$work/$name.err")" > "$work/$name.why"; return 1
    fi
    if ! "$od" -t "$obj" > "$work/$name.sym" 2>> "$work/$name.err"; then
        echo "objdump -t failed: $(head -n 1 "$work/$name.err")" > "$work/$name.why"; return 1
    fi
    grep -E ' F ' "$work/$name.sym" | awk '{print $NF}' > "$work/$name.funcs"
    python3 "$scanner" "$work/$name.dis" --funcs "$work/$name.funcs" > "$work/$name.out"; rc=$?
    witnessed=$(sed -n "s/^witnessed counted loops ([^)]*): *//p" "$work/$name.out")
    if [ $rc -ne 0 ] && [ $rc -ne 3 ]; then echo "scanner exit $rc" > "$work/$name.why"; return 1; fi
    if [ -z "$witnessed" ] || [ "$witnessed" -lt 1 ]; then echo "no CTR-controlled loop (witnessed=${witnessed:-none}): no judged mtctr reaches a bd* branch" > "$work/$name.why"; return 1; fi
    if [ $rc -ne 0 ]; then echo "findings: $(grep '^finding' "$work/$name.out" | head -n 2 | tr '\n' ' ')" > "$work/$name.why"; return 1; fi
    return 0
}

# judge's own controls, with a stand-in objdump that prints a fixed listing: a clean bdnz loop passes; listings
# whose mtctr reaches no bd* branch - only bctrl, only a conditional beq, or the listing ends - fail as "no
# CTR-controlled loop"; and a good listing from an objdump that then exits 1 fails on the objdump status.
fake_od() { # <name> <listing file> <exit>
    printf '#!/bin/sh\nif [ "$1" = -t ]; then echo "0000000000000000 g     F .text\t0000000000000020 .f"; else cat "%s"; fi\nexit %s\n' "$2" "$3" > "$work/$1"
    chmod +x "$work/$1"
}
printf '0000000000000000 <.f>:\n   0:\t79 4a 00 20 \tclrldi  r10,r10,32\n   4:\t7d 49 03 a6 \tmtctr   r10\n   8:\t42 00 00 00 \tbdnz    8 <.f+0x8>\n   c:\t4e 80 00 20 \tblr\n' > "$work/loop.dis"
printf '0000000000000000 <.f>:\n   0:\t7c a9 2a 14 \tadd     r5,r9,r5\n   4:\t7c a9 03 a6 \tmtctr   r5\n   8:\t4e 80 04 21 \tbctrl\n   c:\t4e 80 00 20 \tblr\n' > "$work/nonloop.dis"
printf '0000000000000000 <.f>:\n    0:\t38 a0 00 01 \tli      r5,1\n    4:\t7c a9 03 a6 \tmtctr   r5\n    8:\t41 82 00 04 \tbeq     c <.f+0xc>\n    c:\t4e 80 00 20 \tblr\n' > "$work/condonly.dis"
printf '0000000000000000 <.f>:\n    0:\t38 a0 00 01 \tli      r5,1\n    4:\t7c a9 03 a6 \tmtctr   r5\n' > "$work/truncated.dis"
fake_od od-loop "$work/loop.dis" 0; fake_od od-nonloop "$work/nonloop.dis" 0; fake_od od-fail "$work/loop.dis" 1
fake_od od-cond "$work/condonly.dis" 0; fake_od od-trunc "$work/truncated.dis" 0
if judge "$work/od-loop" /dev/null c-loop && ! judge "$work/od-nonloop" /dev/null c-nonloop &&
   grep -q "no CTR-controlled loop" "$work/c-nonloop.why" && ! judge "$work/od-cond" /dev/null c-cond &&
   grep -q "no CTR-controlled loop" "$work/c-cond.why" && ! judge "$work/od-trunc" /dev/null c-trunc &&
   grep -q "no CTR-controlled loop" "$work/c-trunc.why" && ! judge "$work/od-fail" /dev/null c-fail &&
   grep -q "objdump -d failed" "$work/c-fail.why"; then
    echo "ilp32-doloop-codegen: ok   control: judge passes a bdnz loop; refuses bctrl-only, conditional-only (beq) and truncated listings and a failed objdump"
else
    echo "ilp32-doloop-codegen: FAIL control: judge (loop/nonloop/objdump-fail)"
    for c in c-loop c-nonloop c-cond c-trunc c-fail; do [ -f "$work/$c.why" ] && echo "    $c: $(cat "$work/$c.why")"; done; status=1
fi

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
        if judge "$objdump" "$work/$name.o" "$name"; then
            echo "ilp32-doloop-codegen: ok   ${model:-ILP32} $opt: $(sed -n "s/^witnessed counted loops ([^)]*): *//p" "$work/$name.out") CTR-controlled loop(s), every possible CTR count has a defined upper half"
        else
            echo "ilp32-doloop-codegen: FAIL ${model:-ILP32} $opt: $(cat "$work/$name.why")"; cat "$work/$name.out" 2>/dev/null; status=1
        fi
    done
done

[ $status -eq 0 ] && echo "ilp32-doloop-codegen: PASS" || echo "ilp32-doloop-codegen: FAIL"
exit $status
