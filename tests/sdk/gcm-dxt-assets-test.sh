#!/bin/sh
# hello-ppu-gcm-dxt embeds tools/dxt-encoder/dxt-example output: rebuild the
# example, regenerate the header and require the committed copy to match.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
sample="$root/samples/gcm/hello-ppu-gcm-dxt"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "gcm-dxt-assets: FAIL: $*" >&2; exit 1; }

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/tools/dxt-encoder" \
    "$root/tools/dxt-encoder/example.c" "$root/tools/dxt-encoder/dxt.c" -o "$tmp/dxt-example"
(cd "$tmp" && ./dxt-example input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5 >/dev/null) ||
    fail "dxt-example failed"
(cd "$tmp" && "${PYTHON:-python3}" "$sample/generate-assets.py" \
    input.rgba tiles.dxt1 tiles.dxt3 tiles.dxt5 assets.h) || fail "generate-assets.py failed"
cmp -s "$tmp/assets.h" "$sample/source/assets.h" ||
    fail "source/assets.h differs from regenerated dxt-example output (see the sample README)"
echo "gcm-dxt-assets: PASS (source/assets.h matches dxt-example output)"
