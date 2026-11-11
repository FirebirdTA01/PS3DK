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

build() {
    local out=$1; shift
    # The PSL1GHT-derived emitters are built as the SDK builds them: without
    # -Werror and with their implicit-declaration and parameter warnings off.
    "$cc" -std=gnu11 -O1 -w -ffunction-sections -fdata-sections "${san[@]}" "${inc[@]}" -fno-sanitize=shift -DLIBRSX_INTERNAL \
        -c "$src/rsx_legacy/commands.c" -o "$out-commands.o"
    "$cc" -std=gnu11 -O1 -w -ffunction-sections -fdata-sections "${san[@]}" "${inc[@]}" \
        -c "$src/gcm_rsx_wrappers.c" -o "$out-wrappers.o"
    "$cc" -std=gnu11 -O1 -Wall -Wextra -Werror "${san[@]}" "${inc[@]}" "$@" \
        -DPS3TC_GCM_SEGMENT_HOST_TEST -c "$src/gcm_segment.c" -o "$out-segment.o"
    "$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers "${san[@]}" "${inc[@]}" ${test_defs:-} \
        -c "$repo_root/tests/sdk/gcm-segment-test.c" -o "$out-test.o"
    "$cc" "${san[@]}" -Wl,--gc-sections "$out-test.o" "$out-segment.o" "$out-wrappers.o" "$out-commands.o" -o "$out"
}

build "$tmp/fixed"
rc=0; "$tmp/fixed" || rc=$?
[[ "$rc" -eq 0 ]] || fail "segment helper: expected exit 0, got $rc"

build "$tmp/control" -DSEGMENT_CONTROL_WRAP
rc=0; "$tmp/control" > "$tmp/control.log" 2>&1 || rc=$?
cat "$tmp/control.log"
[[ "$rc" -eq 1 ]] || fail "wrapping control: expected exit 1, got $rc"
grep -q 'FAIL refused' "$tmp/control.log" || fail "wrapping control did not fail the refusal check"
echo "control: a wrapping callback fails the overflow checks (exit 1)"

# The observed build (libgcm_cmd_prof_auto.a) tells the automatic observer
# which context is a segment context; without the seam the same test must fail.
test_defs=-DTEST_AUTO_SEGMENT build "$tmp/auto" -DPS3TC_PROF_AUTO=1
rc=0; "$tmp/auto" > "$tmp/auto.log" 2>&1 || rc=$?
cat "$tmp/auto.log"
[[ "$rc" -eq 0 ]] || fail "observed segment seam: expected exit 0, got $rc"
grep -q 'auto seam: open/close bracket every PPU-written segment' "$tmp/auto.log" || fail "observed seam not exercised"
test_defs=-DTEST_AUTO_SEGMENT build "$tmp/auto-control"
rc=0; "$tmp/auto-control" > "$tmp/auto-control.log" 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || fail "unobserved seam control: expected exit 1, got $rc"
grep -q 'FAIL auto open' "$tmp/auto-control.log" || fail "unobserved control did not fail the seam check"
echo "control: the default build reports no segment contexts (exit 1)"

echo "gcm-segment-test: PASS"
