#!/bin/sh
# Host S3TC format regression: byte fixtures, decoded alpha/color and sizes.
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
