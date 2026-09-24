#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
source_file=${EXIT_SOURCE:-$root/runtime/lv2/librt/exit.c}
extra=()
if grep -q '__librt_register_fini' "$source_file"; then extra+=(-DHAS_REGISTER_FINI); fi
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$root/tests/sdk/exit-fixture" "-DEXIT_SOURCE=\"$source_file\"" \
    "${extra[@]}" "$root/tests/sdk/librt-exit-test.c" -o "$work/exit-test"
"$work/exit-test"
