#!/usr/bin/env bash
# Host test for the S3TC DXT1/DXT3/DXT5 encoder (t_a229ec8f patch 3).
#
# Compiles and runs the C test, which:
#   * decodes the encoder's 8-byte (DXT1) / 16-byte (DXT3/DXT5) blocks
#     with an INDEPENDENT decoder (not linked from dxt.c), and checks
#     exact round-trip on solid blocks where 5:6:5 / 4-bit / DXT5-ramp
#     are all lossless (colours {0,255}, alpha {0,240});
#   * checks a linear gradient stays within a small per-channel bound
#     (the DXT1 bounding-box limit on this input is ~170, not garbage
#     which would be ~255);
#   * verifies output sizes (8/16 bytes per 4x4 block).
#
# Host-only; no cross compiler needed.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "dxt-encoder: FAIL: $*" >&2; exit 1; }

[ -f "$root/tools/dxt-encoder/dxt.c" ] || fail "dxt.c missing"
[ -f "$root/tools/dxt-encoder/dxt.h" ] || fail "dxt.h missing"

"$CC" -std=c11 -Wall -Wextra -Werror \
    -I"$root/tools/dxt-encoder" \
    "$root/tests/sdk/dxt-encoder-test.c" \
    "$root/tools/dxt-encoder/dxt.c" \
    -o "$tmp/dxttest"

out=$("$tmp/dxttest")
printf '%s\n' "$out" | sed 's/^/dxt-encoder: /'

case "$out" in
    *"PASS"*) echo "dxt-encoder: PASS" ;;
    *)        fail "the test binary reported a failure (see above)" ;;
esac
