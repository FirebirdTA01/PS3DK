#!/bin/sh
# Portable framebuffer checks used by the rendering samples.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
    "$root/tests/sdk/gcm-render-pixels-test.c" -o "$tmp/pixels"
"$tmp/pixels"
