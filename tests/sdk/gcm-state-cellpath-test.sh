#!/usr/bin/env bash
# gcm-state-cellpath-test.sh — verifies the PUBLIC cellGcm* forwarders
# (from <cell/gcm.h> -> gcm_command_c.h) emit the same NV40 words as the
# shared natives in <cell/gcm/gcm_state_native.h>.  Catches a missing or
# duplicated native, or a forwarder mis-wiring, that the native-direct
# test (gcm-state-native-test.c) would not see.
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
      "$root/tests/sdk/gcm-state-cellpath-host.c" -o "$WORK/test"
"$WORK/test"
