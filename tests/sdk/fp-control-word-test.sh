#!/usr/bin/env bash
# The SDK's fragment-program control word has ONE builder
# (sdk/include/cell/gcm/gcm_fp_control.h) and three consumers.  This test:
#   1. refuses any second copy of the builder's arithmetic under sdk/ and
#      requires each consumer to call the shared function - the 2026-09-06
#      defect was a fix landing in one copy and missing another in the
#      same file;
#   2. compiles the expectation table against the PARENT formula (the
#      builder as it stood in the immediately preceding commit) and requires
#      it to FAIL on exactly the rows marked {h0}: a table that cannot fail
#      pins nothing.  The MARK MOVES with each commit that changes the
#      builder - it was {depth} when depth forwarding landed - and the
#      invariant is "fails exactly", never "fails at least";
#   3. compiles it against the real header and requires every row to pass.
# Host-only: the builder depends on nothing but <stdint.h>.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-cc}
src="$root/tests/sdk/fp-control-word-test.c"
inc="$root/sdk/include"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "fp-control-word-test: FAIL: $*" >&2; exit 1; }

# 1. one builder, three callers
# The pattern accepts ANY hex constant against 0x40 so it matches both
# spellings.  The H0 branch's constant changed from 0x0e to 0x00
# (t_96daf53b); a literal '0x0e' pattern would still have caught a stale copy
# written at the old spelling, but not one written at the new one, and the
# point of this check is to catch a copy whatever it says.
dups=$(grep -rl --include='*.h' --include='*.c' --include='*.cpp' \
        -E '\? *0x[0-9a-fA-F]+u? *: *0x40u?' "$root/sdk" | grep -v '/gcm_fp_control\.h$' || true)
[ -z "$dups" ] || fail "a second FP_CONTROL builder lives outside gcm_fp_control.h: $dups"
bridge="$root/sdk/include/cell/gcm/gcm_cg_bridge.h"
psgl="$root/sdk/libPSGL/src/psgl_bootstrap.c"
grep -q 'ps3tc_fp_control_word(' "$bridge"     || fail "cellGcmSetFragmentProgram does not call ps3tc_fp_control_word"
grep -q 'ps3tc_fp_control_from_cgb(' "$bridge" || fail "ps3tc_cgb_fp_control does not call ps3tc_fp_control_from_cgb"
grep -q 'ps3tc_fp_control_from_cgb(' "$psgl"   || fail "psgl_bootstrap_fp_control does not call ps3tc_fp_control_from_cgb"
grep -q 'gcm_fp_control.h' "$root/sdk/Makefile" || fail "gcm_fp_control.h is not in the SDK's installed header list"

# 2. negative run: the parent formula must fail exactly the {h0} rows
"$CC" -std=c99 -Wall -Wextra -Werror -DFP_CONTROL_PARENT_FORMULA -I"$inc" \
      "$src" -o "$tmp/parent"
if "$tmp/parent" > "$tmp/parent.log"; then
    cat "$tmp/parent.log"; fail "the parent formula passed the table: the table cannot fail"
fi
expected_fail=$(grep -c '{h0}' "$tmp/parent.log")
got_fail=$(grep -c '^FAIL' "$tmp/parent.log")
wrong=$(grep '^FAIL' "$tmp/parent.log" | grep -vc '{h0}' || true)
[ "$expected_fail" -gt 0 ] || fail "no {h0} rows in the table"
[ "$got_fail" = "$expected_fail" ] && [ "$wrong" = 0 ] || {
    cat "$tmp/parent.log"
    fail "parent formula failed $got_fail rows, $wrong of them not {h0}; expected exactly the $expected_fail {h0} rows"
}
echo "fp-control-word-test: negative run: parent formula fails exactly the $expected_fail {h0} rows"

# 3. the real builder passes every row
"$CC" -std=c99 -Wall -Wextra -Werror -I"$inc" "$src" -o "$tmp/real"
"$tmp/real" | tee "$tmp/real.log"
grep -q '^FAIL' "$tmp/real.log" && fail "the shared builder fails a row"
# No artefact count here.  Every row is an assertion now that H0-alone and
# H0+DEPTH are distinct words, and the count that used to be printed was
# itself a trap: `grep -c` returns 1 when it matches nothing, so under set -e
# the script died after printing "15/15" and never reached this line - a test
# that passed its own table and still exited non-zero.
echo "fp-control-word-test: PASS"
