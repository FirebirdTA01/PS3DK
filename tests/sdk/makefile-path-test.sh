#!/usr/bin/env bash
# The PSL1GHT-style Makefile build path, end to end, from an installed SDK.
#
# Most third-party PS3 homebrew builds with a Makefile that includes
# $(PSL1GHT)/ppu_rules, calls the toolchain by its short ppu-* names, links
# data in through bin2o (bin2s) and makes .self and .pkg files with
# sprxlinker, make_self, fself, make_self_npdrm, sfo, pkg and
# package_finalize.  Before this test, a release could ship without make
# (Windows), without the ppu-* names (Windows) or without most of those
# tools (the Linux tools tarball), and nothing noticed.
#
# Usage: tests/sdk/makefile-path-test.sh [SDK_ROOT]
#   SDK_ROOT defaults to $PS3DK.  On Windows run it from Git Bash in a shell
#   where %PS3DK%\setup.cmd has set PATH (so make and the tools resolve the
#   way a user's would); on Linux with $PS3DEV/bin and $PS3DEV/ppu/bin on PATH.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
sdk="${1:-${PS3DK:-}}"
if [ -z "$sdk" ]; then
    echo "makefile-path-test: SKIP (set PS3DK or pass an installed SDK root)"
    exit 0
fi
[ -f "$sdk/ppu_rules" ] || { echo "FAIL: $sdk has no ppu_rules" >&2; exit 2; }

note() { printf 'makefile-path-test: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

for tool in make ppu-gcc ppu-ld ppu-strip bin2s sprxlinker make_self fself \
            make_self_npdrm sfo pkg package_finalize; do
    command -v "$tool" >/dev/null 2>&1 || fail "'$tool' is not on PATH"
    note "found $tool: $(command -v "$tool")"
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
proj="$tmp/mkpath"
cp -R "$repo_root/tests/sdk/fixtures/makefile-path" "$proj"

# Keep what setup.cmd (or the user's shell) exported: on Windows those are the
# forward-slash forms the rules need inside sh recipes.
export PS3DK="${PS3DK:-$sdk}" PS3DEV="${PS3DEV:-$sdk}" PSL1GHT="${PSL1GHT:-$sdk}"
note "PS3DEV=$PS3DEV PSL1GHT=$PSL1GHT"
note "make (elf, self)"
make -C "$proj" > "$tmp/make.log" 2>&1 || { cat "$tmp/make.log"; fail "make failed"; }
note "make pkg"
make -C "$proj" pkg > "$tmp/pkg.log" 2>&1 || { cat "$tmp/pkg.log"; fail "make pkg failed"; }

for out in mkpath.elf mkpath.self mkpath.fake.self mkpath.pkg mkpath.gnpdrm.pkg; do
    [ -s "$proj/$out" ] || { cat "$tmp/make.log" "$tmp/pkg.log"; fail "missing output $out"; }
    note "built $out ($(wc -c < "$proj/$out") bytes)"
done

# The data blob must be linked in, not just assembled.
ppu-nm "$proj/mkpath.elf" 2>/dev/null | grep -q ' blob_bin$' \
    || fail "blob_bin symbol (from bin2o) is not in mkpath.elf"
# The pkg must carry the CONTENTID the Makefile set.
grep -aq 'UP0001-MKPT00001_00-0000000000000000' "$proj/mkpath.pkg" \
    || fail "mkpath.pkg does not carry the Makefile's CONTENTID"

note "PASS"
