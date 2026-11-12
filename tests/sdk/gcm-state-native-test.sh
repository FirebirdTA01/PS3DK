#!/usr/bin/env bash
# gcm-state-native-test.sh — byte-identical guard (aarch64 / x86 host).
#
# Compiles tests/sdk/gcm-state-native-test.c against the real SDK
# headers.  No per-SDK shim file: a non-PowerPC host neutralizes the
# PowerPC-only ATTRIBUTE_PRXPTR pointer mode via `-DATTRIBUTE_PRXPTR=`;
# sdk/include/ppu-types.h carries a matching `#ifndef` guard.  <cell/gcm.h>
# and therefore ps3tc_gcm_reserve (the single ring-full + callback path
# the new SAFE natives reuse) host-compile under that setup, so the test
# drives the REAL shared reserve, not a copy.
#
# Covers logic-op, depth, blend, dither, stencil, and raster-state natives
# (safe + unsafe each), byte-for-byte vs the pre-patch rsx* bodies,
# plus ring-full / callback cases per native and a public cellpath test.
# No expected hex is hard-coded.

set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-cc}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# Guard must be in the header we compile against.
grep -q '#ifndef ATTRIBUTE_PRXPTR' "$root/sdk/include/ppu-types.h" \
    || { echo "gcm-state-native-test: FAIL: ppu-types.h lacks the #ifndef guard for ATTRIBUTE_PRXPTR"; exit 1; }

"$CC" -std=gnu11 -Wall -Wextra -Werror -O2 \
      -DATTRIBUTE_PRXPTR= \
      -I"$root/sdk/include" -I"$root/sdk/libgcm_cmd/include" \
      "$root/tests/sdk/gcm-state-native-test.c" -o "$WORK/test"
"$WORK/test"
