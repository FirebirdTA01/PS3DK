#!/usr/bin/env bash
# GCM command segments: cellGcmSetupContextData and the segment helper.
#
# Builds tests/sdk/gcm-segment-test.c with a host C compiler together with
# the real SDK sources it checks - the rsx emitters
# (sdk/libgcm_cmd/src/rsx_legacy/commands.c), the cellGcm link wrappers
# (gcm_rsx_wrappers.c, home of cellGcmSetupContextData) and
# sdk/libgcm_cmd/src/gcm_segment.c - and runs it.  No PS3 toolchain.
#
# Host shims, in a temporary directory placed first on the include path:
# ppu-types.h drops the 32-bit pointer mode (the PPU ILP32 layout is not
# what this test checks), ppc-asm.h / ppu_intrinsics.h are empty, and
# sys/lv2_types.h passes callback pointers through at full width.
#
# A control build (-DSEGMENT_CONTROL_WRAP) replaces the segment's
# refusing callback with one that wraps to the buffer start, the way a
# ring callback does; it must fail the overflow checks (exit 1), which
# shows the test can see a silent wrap.
#
# Then mutants, each undoing one review fix in a copy of gcm_segment.c,
# must exit 1 and fail the checks that cover that fix.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

shim="$tmp/shim"
mkdir -p "$shim/sys"
printf '#include_next <ppu-types.h>\n#undef ATTRIBUTE_PRXPTR\n#define ATTRIBUTE_PRXPTR\n' > "$shim/ppu-types.h"
: > "$shim/ppc-asm.h"
printf '#define __cntlzw(x) ((x) ? (unsigned)__builtin_clz(x) : 32u)
' > "$shim/ppu_intrinsics.h"
cat > "$shim/sys/lv2_types.h" <<'EOF'
#define lv2_fn_to_callback_ea lv2_fn_to_callback_ea_target
#include_next <sys/lv2_types.h>
#undef lv2_fn_to_callback_ea
#define lv2_fn_to_callback_ea(fn) ((uintptr_t)(fn))
EOF

san=()
if "$cc" -fsanitize=address,undefined -x c /dev/null -c -o "$tmp/probe.o" >/dev/null 2>&1; then
    san=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi
inc=(-I "$shim" -I "$repo_root/sdk/include" -I "$repo_root/sdk/include/rsx"
     -I "$repo_root/sdk/libgcm_cmd/src/rsx_legacy" -I "$repo_root/sdk/libgcm_cmd/include")
src="$repo_root/sdk/libgcm_cmd/src"

build() {   # build OUTPUT SEGMENT-SOURCE [extra segment flags]
    local out=$1 seg=$2; shift 2
    # The PSL1GHT-derived emitters are built as the SDK builds them: without
    # -Werror and with their implicit-declaration and parameter warnings off.
    "$cc" -std=gnu11 -O1 -w -ffunction-sections -fdata-sections "${san[@]}" "${inc[@]}" -fno-sanitize=shift -DLIBRSX_INTERNAL \
        -c "$src/rsx_legacy/commands.c" -o "$out-commands.o"
    "$cc" -std=gnu11 -O1 -w -ffunction-sections -fdata-sections "${san[@]}" "${inc[@]}" \
        -c "$src/gcm_rsx_wrappers.c" -o "$out-wrappers.o"
    "$cc" -std=gnu11 -O1 -Wall -Wextra -Werror "${san[@]}" "${inc[@]}" "$@" \
        -DPS3TC_GCM_SEGMENT_HOST_TEST -c "$seg" -o "$out-segment.o"
    "$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers "${san[@]}" "${inc[@]}" \
        -c "$repo_root/tests/sdk/gcm-segment-test.c" -o "$out-test.o"
    "$cc" "${san[@]}" -Wl,--gc-sections "$out-test.o" "$out-segment.o" "$out-wrappers.o" "$out-commands.o" -o "$out"
}

build "$tmp/fixed" "$src/gcm_segment.c"
rc=0; "$tmp/fixed" || rc=$?
[[ "$rc" -eq 0 ]] || fail "segment helper: expected exit 0, got $rc"

build "$tmp/control" "$src/gcm_segment.c" -DSEGMENT_CONTROL_WRAP
rc=0; "$tmp/control" > "$tmp/control.log" 2>&1 || rc=$?
cat "$tmp/control.log"
[[ "$rc" -eq 1 ]] || fail "wrapping control: expected exit 1, got $rc"
grep -q 'FAIL refused' "$tmp/control.log" || fail "wrapping control did not fail the refusal check"
echo "control: a wrapping callback fails the overflow checks (exit 1)"

# mutant NAME SED-EXPR CHECK... : the helper with one review fix undone must
# exit 1 and fail each named check.
mutant() {
    local name=$1 expr=$2; shift 2
    sed -e "$expr" "$src/gcm_segment.c" > "$tmp/$name.c"
    cmp -s "$tmp/$name.c" "$src/gcm_segment.c" && fail "mutant $name: edit did not apply"
    build "$tmp/$name" "$tmp/$name.c"
    rc=0; "$tmp/$name" > "$tmp/$name.log" 2>&1 || rc=$?
    [[ "$rc" -eq 1 ]] || { cat "$tmp/$name.log"; fail "mutant $name: expected exit 1, got $rc"; }
    for check in "$@"; do
        grep -q "FAIL $check:" "$tmp/$name.log" || { cat "$tmp/$name.log"; fail "mutant $name: '$check' did not fail"; }
    done
    echo "mutant $name: fails $* (exit 1)"
}

# Call reserving only the CALL's word: the label is reserved on its own again.
mutant call-split 's/^#define SEGMENT_CALL_WORDS 5u$/#define SEGMENT_CALL_WORDS 1u/'     "refused busy" "ring words kept" "segment kept" "call first"
# Init accepting RSX local memory again.
mutant local-init 's/^        location != GCM_LOCATION_CELL)$/        (location != GCM_LOCATION_CELL \&\& location != GCM_LOCATION_RSX))/'     "local refused" "segment untouched" "label untouched"

echo "gcm-segment-test: PASS"
