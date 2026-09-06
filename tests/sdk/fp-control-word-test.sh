#!/usr/bin/env bash
# The SDK's fragment-program control word has ONE builder
# (sdk/include/cell/gcm/gcm_fp_control.h) and three consumers.  This test:
#   1. refuses any second copy of the builder's arithmetic under sdk/ and
#      requires each consumer to call the shared function - the 2026-09-06
#      defect was a fix landing in one copy and missing another in the
#      same file;
#   2. compiles the expectation table against the PARENT formula (the
#      builder as it stood before the fix) and requires it to FAIL on
#      exactly the rows marked {depth}: a table that cannot fail pins
#      nothing;
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
dups=$(grep -rl --include='*.h' --include='*.c' --include='*.cpp' \
        -E '\? *0x0eu? *: *0x40u?' "$root/sdk" | grep -v '/gcm_fp_control\.h$' || true)
[ -z "$dups" ] || fail "a second FP_CONTROL builder lives outside gcm_fp_control.h: $dups"
bridge="$root/sdk/include/cell/gcm/gcm_cg_bridge.h"
psgl="$root/sdk/libPSGL/src/psgl_bootstrap.c"
grep -q 'ps3tc_fp_control_word(' "$bridge"     || fail "cellGcmSetFragmentProgram does not call ps3tc_fp_control_word"
grep -q 'ps3tc_fp_control_from_cgb(' "$bridge" || fail "ps3tc_cgb_fp_control does not call ps3tc_fp_control_from_cgb"
grep -q 'ps3tc_fp_control_from_cgb(' "$psgl"   || fail "psgl_bootstrap_fp_control does not call ps3tc_fp_control_from_cgb"
grep -q 'gcm_fp_control.h' "$root/sdk/Makefile" || fail "gcm_fp_control.h is not in the SDK's installed header list"

# 2. negative run: the parent formula must fail exactly the {depth} rows
"$CC" -std=c99 -Wall -Wextra -Werror -DFP_CONTROL_PARENT_FORMULA -I"$inc" \
      "$src" -o "$tmp/parent"
if "$tmp/parent" > "$tmp/parent.log"; then
    cat "$tmp/parent.log"; fail "the parent formula passed the table: the table cannot fail"
fi
expected_fail=$(grep -c '{depth}' "$tmp/parent.log")
got_fail=$(grep -c '^FAIL' "$tmp/parent.log")
wrong=$(grep '^FAIL' "$tmp/parent.log" | grep -vc '{depth}' || true)
[ "$expected_fail" -gt 0 ] || fail "no {depth} rows in the table"
[ "$got_fail" = "$expected_fail" ] && [ "$wrong" = 0 ] || {
    cat "$tmp/parent.log"
    fail "parent formula failed $got_fail rows, $wrong of them not {depth}; expected exactly the $expected_fail {depth} rows"
}
echo "fp-control-word-test: negative run: parent formula fails exactly the $expected_fail {depth} rows"

# 3. the real builder passes every row
"$CC" -std=c99 -Wall -Wextra -Werror -I"$inc" "$src" -o "$tmp/real"
"$tmp/real" | tee "$tmp/real.log"
grep -q '^FAIL' "$tmp/real.log" && fail "the shared builder fails a row"
artefacts=$(grep -c '\[artefact\]' "$tmp/real.log")
echo "fp-control-word-test: PASS ($artefacts artefact row(s) recorded, not asserted: see the table's header)"
