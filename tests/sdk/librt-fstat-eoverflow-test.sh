#!/usr/bin/env bash
# librt stat()/fstat() report EOVERFLOW for a file larger than off_t holds.
#
# Builds librt-fstat-eoverflow-test.c, which includes the real
# runtime/lv2/librt/fstat.c, against fstat-eoverflow-fixture/ (first on the
# include path: a 32-bit off_t as in the PPU ABI, a stand-in LV2 stat
# syscall and path resolver).  Cases: 3 GiB through fstat and stat must fail
# with EOVERFLOW; INT32_MAX and 1 MiB must succeed with the size copied.
# Exit status is asserted exactly.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
fixture="$root/tests/sdk/fstat-eoverflow-fixture"
src="$root/runtime/lv2/librt/fstat.c"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$cc" -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$fixture" -I"$root/runtime/lv2/librt" \
    "-DST_size_SOURCE=\"$src\"" \
    "$root/tests/sdk/librt-fstat-eoverflow-test.c" -o "$tmp/test"

rc=0; "$tmp/test" || rc=$?
[[ "$rc" -eq 0 ]] || fail "expected exit 0, got $rc"
echo "librt-fstat-eoverflow-test: PASS"
