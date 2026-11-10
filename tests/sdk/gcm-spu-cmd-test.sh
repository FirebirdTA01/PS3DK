#!/usr/bin/env bash
# SPU GCM command writer: byte-exact against the PPU emitters.
#
# Builds sdk/libgcm_spu_cmd/src/gcm_spu_cmd.c for the host with its
# symbols renamed (spuw_*), links it with the PPU emitters it mirrors (the
# rsx_legacy commands.c object, the Cg / texture bridges and the SPU
# prepare helpers, which tests/sdk/gcm-spu-cmd-test.c includes), and runs
# the comparison of every call.  No PS3 toolchain.
#
# Then a mutant of the writer with one method address changed (scissor)
# must fail exactly the two scissor cases, which shows the comparison sees
# a one-word difference.
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
printf '#define __cntlzw(x) ((x) ? (unsigned)__builtin_clz(x) : 32u)\n' > "$shim/ppu_intrinsics.h"
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
rename=('-DCELL_GCM_SPU_FN(n)=spuw_##n' '-DCELL_GCM_SPU_RSX(n)=spuw_rsx_##n')
ppu_inc=(-I "$shim" -I "$repo_root/sdk/include" -I "$repo_root/sdk/include/rsx"
         -I "$repo_root/sdk/libgcm_cmd/src/rsx_legacy" -I "$repo_root/sdk/libgcm_cmd/include")
# Only the writer's header: sdk/include-spu also holds SPU-only libc headers.
mkdir -p "$tmp/spuinc/cell/gcm"
cp "$repo_root/sdk/include-spu/cell/gcm/gcm_spu_cmd.h" "$tmp/spuinc/cell/gcm/"
spu_inc=(-I "$tmp/spuinc")

# The PSL1GHT-derived emitters shift promoted 16-bit values into bit 31
# (blend equations); -fno-sanitize=shift keeps that out of the UBSan run.
"$cc" -std=gnu11 -O1 -w -ffunction-sections -fdata-sections "${san[@]}" "${ppu_inc[@]}" -fno-sanitize=shift -DLIBRSX_INTERNAL \
    -c "$repo_root/sdk/libgcm_cmd/src/rsx_legacy/commands.c" -o "$tmp/commands.o"
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-function \
    "${san[@]}" "${rename[@]}" "${spu_inc[@]}" "${ppu_inc[@]}" \
    -c "$repo_root/tests/sdk/gcm-spu-cmd-test.c" -o "$tmp/test.o"

writer() {   # writer SOURCE OUTPUT
    "$cc" -std=gnu11 -O1 -Wall -Wextra -Werror "${san[@]}" "${rename[@]}" "${spu_inc[@]}" \
        -c "$1" -o "$2-writer.o"
    "$cc" "${san[@]}" -Wl,--gc-sections "$tmp/test.o" "$2-writer.o" "$tmp/commands.o" -o "$2"
}

writer "$repo_root/sdk/libgcm_spu_cmd/src/gcm_spu_cmd.c" "$tmp/fixed"
rc=0; "$tmp/fixed" || rc=$?
[[ "$rc" -eq 0 ]] || fail "SPU writer: expected exit 0, got $rc"

sed -e 's/SCISSOR_HORIZ = 0x08c0/SCISSOR_HORIZ = 0x08c4/' \
    "$repo_root/sdk/libgcm_spu_cmd/src/gcm_spu_cmd.c" > "$tmp/mutant.c"
cmp -s "$tmp/mutant.c" "$repo_root/sdk/libgcm_spu_cmd/src/gcm_spu_cmd.c" && fail "mutant edit did not apply"
writer "$tmp/mutant.c" "$tmp/mutant"
rc=0; "$tmp/mutant" > "$tmp/mutant.log" 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || { cat "$tmp/mutant.log"; fail "scissor mutant: expected exit 1, got $rc"; }
failed=$(grep '^  FAIL' "$tmp/mutant.log" | awk '{print $2}' | sort -u | wc -l)
[[ "$failed" -eq 2 ]] && grep -q 'FAIL scissor ' "$tmp/mutant.log" && grep -q 'FAIL rsx_scissor ' "$tmp/mutant.log" \
    || { cat "$tmp/mutant.log"; fail "scissor mutant: expected exactly the two scissor cases to fail, got $failed"; }
echo "mutant: a changed scissor method fails exactly the two scissor cases (exit 1)"

echo "gcm-spu-cmd-test: PASS"
