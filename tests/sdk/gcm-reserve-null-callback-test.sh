#!/usr/bin/env bash
# gcm-reserve-null-callback-test.sh — pins the safe-path reserve contract in
# the SHARED ps3tc_gcm_reserve helper (gcm_cg_bridge.h), exercised through the
# public cellGcm* forwarders, a state native, AND a tex native (all three
# families route the same reservation helper, so one fix covers all):
#   - full ring + NULL callback     -> must NOT write, current unchanged
#   - callback refuses              -> must NOT write, current unchanged
#   - callback relocates            -> writes at the relocated address
#   - unsafe natives on full ring   -> unchanged (still write; documented unsafe)
#
# No per-SDK shim file: a non-PowerPC host neutralizes the PowerPC-only
# ATTRIBUTE_PRXPTR pointer mode via `-DATTRIBUTE_PRXPTR=`; sdk/include/
# ppu-types.h carries a matching `#ifndef` guard.

set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-cc}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu11 -Wall -Wextra -Werror -O2 \
      -DATTRIBUTE_PRXPTR= \
      -I"$root/sdk/include" -I"$root/sdk/libgcm_cmd/include" \
      "$root/tests/sdk/gcm-reserve-null-callback-host.c" -o "$WORK/test"
"$WORK/test"
