#!/usr/bin/env bash
# The package's cmake/ directory: samples and users configure with <root>/cmake/ps3-ppu-toolchain.cmake, and the
# helpers it includes sit next to it.  The SDK install can already put files in $PS3DK/cmake (the AUTO profiler package
# installs its helpers there), and packaging once copied the repository's cmake/ INTO that existing directory, which
# shipped cmake/cmake/ and no top-level ps3-self.cmake.
#
#   1. package-windows-release.sh's cmake/ copy block, run on a synthetic stage whose cmake/ already holds the AUTO
#      helpers: the stage's cmake/ ends up equal to the repository's cmake/, with no cmake/cmake.
#   2. check-release-tree.sh's cmake/ section on synthetic trees (the other sections fail on them, as they should):
#      the entry points at the top level -> ok; one missing -> FAIL naming it; a nested cmake/cmake -> FAIL.
#
# usage: release-tree-cmake-layout-test.sh   Exit 0 pass, 1 fail.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
pkg="$root/scripts/package-windows-release.sh"
crt="$root/scripts/check-release-tree.sh"
status=0
ok()   { echo "release-tree-cmake-layout: ok   $*"; }
fail() { echo "release-tree-cmake-layout: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# 1. The packaging copy into an existing cmake/.
block=$(awk '/^if \[\[ -d "\$PS3_TOOLCHAIN_ROOT\/cmake" \]\]; then$/{f=1} f{print} f && /^fi$/{exit}' "$pkg")
if [ -z "$block" ]; then
    fail "cmake/ copy block not found in package-windows-release.sh"
else
    mkdir -p "$work/stage/cmake"
    cp "$root/cmake/ps3-prof-auto.cmake" "$root/cmake/ps3-prof-auto-check.py" "$root/cmake/ps3-prof-auto-identity.py" \
       "$work/stage/cmake/"
    if ( STAGE_DIR="$work/stage"; PS3_TOOLCHAIN_ROOT="$root"
         say() { :; }; warn() { :; }; die() { echo "die: $*"; exit 1; }
         eval "$block" ) > "$work/copy.out" 2>&1; then
        if [ -e "$work/stage/cmake/cmake" ]; then
            fail "copy into an existing cmake/ nested the tree as cmake/cmake"
        elif diff -r "$root/cmake" "$work/stage/cmake" > "$work/copy.diff" 2>&1; then
            ok "copy into an existing cmake/ merges: the stage cmake/ equals the repository cmake/"
        else
            fail "staged cmake/ differs from the repository cmake/"; sed 's/^/    /' "$work/copy.diff" | head -10
        fi
    else
        fail "cmake/ copy block failed"; sed 's/^/    /' "$work/copy.out"
    fi
fi

# 2. check-release-tree.sh's cmake/ section.
section() {
    bash "$crt" "$1" 2>&1 | sed -n '/^-- cmake\/ entry points at the top level --$/,/^-- /p'
}
entry="ps3-ppu-toolchain.cmake ps3-ppu-toolchain-lp64.cmake ps3-spu-toolchain.cmake ps3-self.cmake"
mk() { mkdir -p "$1/cmake"; for f in $entry; do : > "$1/cmake/$f"; done; }

mk "$work/good"
out=$(section "$work/good")
if [ -z "$out" ]; then
    fail "good tree: the cmake/ section did not run"
elif grep -q '^  FAIL' <<< "$out"; then
    fail "good tree: refused"; sed 's/^/    /' <<< "$out"
elif [ "$(grep -c '^  ok' <<< "$out")" -eq 5 ]; then
    ok "good tree: accepted (4 entry points, no nested cmake/cmake)"
else
    fail "good tree: unexpected verdict"; sed 's/^/    /' <<< "$out"
fi

mk "$work/missing"; rm -f "$work/missing/cmake/ps3-self.cmake"
out=$(section "$work/missing")
grep -q '^  FAIL    cmake/ps3-self.cmake missing$' <<< "$out" \
    && ok "missing ps3-self.cmake: refused, naming it" \
    || { fail "missing ps3-self.cmake: not refused"; sed 's/^/    /' <<< "$out"; }

mk "$work/nested"; mkdir -p "$work/nested/cmake/cmake"
out=$(section "$work/nested")
grep -q '^  FAIL    cmake/cmake exists' <<< "$out" \
    && ok "nested cmake/cmake: refused" \
    || { fail "nested cmake/cmake: not refused"; sed 's/^/    /' <<< "$out"; }

[ "$status" -eq 0 ] && echo "PASS: release-tree-cmake-layout-test" || echo "FAIL: release-tree-cmake-layout-test"
exit "$status"
