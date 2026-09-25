#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
fixture="$repo_root/tests/sdk/remove-fixture"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc="${CC:-cc}"
flags=(-std=c11 -Wall -Wextra -Werror -D_REENT_ONLY -D__PPU__ -I"$fixture/include")
"$cc" "${flags[@]}" "$fixture/remove.c" "$repo_root/tests/sdk/newlib-remove-test.c" -o "$tmp/parent"
rc=0
"$tmp/parent" > "$tmp/parent.log" 2>&1 || rc=$?
cat "$tmp/parent.log"
test "$rc" = 1
grep -q 'remove-test: 5 failures' "$tmp/parent.log"
mkdir -p "$tmp/newlib/libc/stdio"
cp "$fixture/remove.c" "$tmp/newlib/libc/stdio/remove.c"
patch -d "$tmp" -p1 < "$repo_root/patches/ppu/newlib-4.x/0016-newlib-remove-directories.patch"
"$cc" "${flags[@]}" "$tmp/newlib/libc/stdio/remove.c" "$repo_root/tests/sdk/newlib-remove-test.c" -o "$tmp/candidate"
"$tmp/candidate"
echo 'newlib-remove-test: PASS (parent red, patched source green)'
