#!/usr/bin/env bash
# Host test for cellGcmConvertSwizzleFormat (patch 2, t_a229ec8f).
#
# Compiles the C test against the converter and runs it.  The test uses an
# independent Morton-2 decoder to verify that every texel lands where
# psgl_morton2 (libPSGL) says it should: 2-D order x->even bits, y->odd bits,
# byte offset = mor(x,y) * texel_bytes within the 16 KiB tile.
#
# Host-only; no cross compiler needed.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "swizzle-convert: FAIL: $*" >&2; exit 1; }

[ -f "$root/sdk/libgcm_cmd/src/convert_swizzle.c" ]       || fail "convert_swizzle.c missing"
[ -f "$root/sdk/include/cell/gcm/gcm_swizzle_func.h" ]    || fail "gcm_swizzle_func.h missing"

"$CC" -std=c11 -Wall -Wextra -Werror \
    -I"$root/sdk/include" \
    "$root/tests/sdk/gcm-swizzle-convert-test.c" \
    "$root/sdk/libgcm_cmd/src/convert_swizzle.c" \
    -o "$tmp/swztest"

out=$("$tmp/swztest")
printf '%s\n' "$out" | sed 's/^/swizzle-convert: /'

case "$out" in
    *"PASS"*) echo "swizzle-convert: PASS" ;;
    *)        fail "the test binary reported a failure (see above)" ;;
esac
