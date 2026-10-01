#!/usr/bin/env bash
# t_9eaa8392: host test for the RSX heap last-error diagnostic.
#
# Compiles sdk/librsx/src/mm.c (byte-identical to
# sdk/libgcm_cmd/src/rsx_legacy/mm.c -- the only thing that varies per
# library is the archive, the source is shared) against a host C
# compiler plus a stub heap/gcmGetConfiguration in
# tests/sdk/rsx-mm-lasterror-test.c, links, and runs.  No PS3 toolchain
# is required: the stubs make heapInit / heapAllocate / heapAllocate-
# Aligned / heapFree and gcmGetConfiguration host-callable.
#
# sdk/include/ppu-types.h defines ATTRIBUTE_PRXPTR as
# __attribute__((mode(SI))) which is only valid on the 64-bit PPU
# targets (powerpc64, sparc64).  On a host x86_64 that attribute is
# rejected.  This script generates a shim directory in $tmp with a
# ppu-types.h that #include_next's the real one and #undef's
# ATTRIBUTE_PRXPTR to empty.  The shim is placed first on the
# -I search path, so only the host build is affected; the PPU build
# (which never includes this shim directory) is untouched.
#
# The test asserts each of the four failure modes (UNINIT, GCM_CONFIG,
# ALLOCATION, FREE) plus the OK sticky-clear contract, and that
# ps3tc_rsx_mm_last_error_string returns a stable non-NULL description
# for every code.  A failure in any mode is a non-zero exit and a
# summary, so -Werror + ASAN/UBSAN in the build flags make it fail hard.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
src="$repo_root/tests/sdk/rsx-mm-lasterror-test.c"
mm_c="$repo_root/sdk/librsx/src/mm.c"
mm_c_gcm="$repo_root/sdk/libgcm_cmd/src/rsx_legacy/mm.c"
inc_rsx="$repo_root/sdk/include"
inc_gcm="$repo_root/sdk/libgcm_cmd/src"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Both mm.c files must be byte-identical: librsx and libgcm_cmd ship the
# same allocator source, and the query a binary resolves is the one in the
# object it linked.  Drifting the copies means the last-error behaviour of
# rsxFree/rsxMalloc depends silently on which archive was first; comparing
# the bytes catches that before it ships (and catches a partial edit to
# one side).
cmp -s "$mm_c" "$mm_c_gcm" || fail "librsx mm.c and libgcm_cmd rsx_legacy mm.c have diverged"

san=()
if "$cc" -fsanitize=address,undefined -x c /dev/null -c -o "$tmp/probe.o" >/dev/null 2>&1; then
    san=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi

# Shim: include the real ppu-types.h, then blank out ATTRIBUTE_PRXPTR so
# gcm_sys.h's `void *localAddress ATTRIBUTE_PRXPTR` field compiles on a
# host.  The shim directory goes first on the -I path below.
shim="$tmp/shim"
mkdir -p "$shim"
cat > "$shim/ppu-types.h" <<'EOF'
#include_next <ppu-types.h>
#undef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
EOF

"$cc" -std=c11 -Wall -Wextra -Werror "${san[@]}" \
    -I "$shim" \
    -I "$inc_rsx" \
    -I "$inc_rsx/rsx" \
    -I "$inc_gcm" \
    "$mm_c" "$src" \
    -o "$tmp/test"

"$tmp/test"

echo "rsx-mm-lasterror-test: PASS"
