#!/usr/bin/env bash
# librt lv2error() maps every LV2 status code to itself (newlib patch 0018:
# the E* errno names are the LV2 codes), so errno holds exactly the code a
# system call returned, including codes without a POSIX name.
#
# Builds librt-lv2errno-test.c, which includes the real
# runtime/lv2/librt/lv2errno.c, against lv2errno-fixture/ (host stand-ins for
# ppu-types.h, sys/reent.h, sys/lv2errno.h).  Red control: the pre-0018
# translator (a 45-entry table with an EINVAL default), embedded below, must
# fail the same test.  Exit status is asserted exactly.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
fixture="$root/tests/sdk/lv2errno-fixture"
cc="${CC:-cc}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
command -v "$cc" >/dev/null 2>&1 || fail "no host C compiler ($cc)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

build() {  # <source> <exe>
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$fixture" \
        "-DLV2ERRNO_SOURCE=\"$1\"" "$root/tests/sdk/librt-lv2errno-test.c" -o "$2"
}

build "$root/runtime/lv2/librt/lv2errno.c" "$tmp/test"
rc=0; "$tmp/test" > "$tmp/out" || rc=$?
[[ "$rc" -eq 0 ]] || { cat "$tmp/out"; fail "expected exit 0, got $rc"; }

# Red control: an LV2 code outside a partial table falls to EINVAL.
cat > "$tmp/old.c" <<'OLD'
#include <errno.h>
#include <sys/reent.h>
#include <ppu-types.h>
#include <sys/lv2errno.h>
s32 lv2error(s32 error)
{
	switch (error) {
	case 0x00000000: return 0;
	case (s32) 0x8001000A: return (s32) 0x8001000A;
	default:         return EINVAL;
	}
}
s32 lv2errno(s32 error) { if (error >= 0) return error; errno = lv2error(error); return -1; }
s32 lv2errno_r(struct _reent *r, s32 error) { if (error >= 0) return error; r->_errno = lv2error(error); return -1; }
OLD
build "$tmp/old.c" "$tmp/red"
rc=0; "$tmp/red" > /dev/null || rc=$?
[[ "$rc" -eq 1 ]] || fail "red control: a partial table should fail (exit 1), got $rc"

echo "librt-lv2errno-test: PASS (red control fails as expected)"
