#!/usr/bin/env bash
# check-release-tree.sh section 6: a release tree must ship samples/ and tests/ as source only.  Packaging from a
# tree in which samples or regression rows were built carries their .self/.elf/.sprx/.prx and CMake build files into
# the zip (a 2026-10-16 GCC 16 packaging dry run shipped 28 prebuilt .fake.self files).
#
# Runs check-release-tree.sh on small synthetic trees and reads only section 6's verdict (the other sections fail on
# a synthetic tree, as they should):
#   clean sources                       -> "ok      no .self/.elf/..."
#   a .fake.self beside a sample        -> FAIL naming it
#   a .elf / .sprx / .prx under tests/  -> FAIL
#   CMakeCache.txt, build.ninja, a CMakeFiles/ dir in a sample build dir -> FAIL
#   no samples/ or tests/ at all        -> ok (nothing shipped, nothing to leak)
#
# usage: release-tree-build-outputs-test.sh   Exit 0 pass, 1 fail.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
crt="$root/scripts/check-release-tree.sh"
status=0
ok()   { echo "release-tree-build-outputs: ok   $*"; }
fail() { echo "release-tree-build-outputs: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# section6 <tree>: the section-6 verdict lines of check-release-tree.sh on <tree>.
section6() {
    bash "$crt" "$1" 2>&1 | sed -n '/^-- no build outputs under samples\/ and tests\/ --$/,/^$/p'
}
# expect <label> <tree> ok|fail [<name that the FAIL must list>]
expect() {
    local label=$1 tree=$2 want=$3 name=${4:-} out
    out=$(section6 "$tree")
    if [ -z "$out" ]; then fail "$label: section 6 did not run"; return; fi
    if [ "$want" = ok ]; then
        grep -q '^  ok      no \.self' <<< "$out" && ok "$label: accepted" || { fail "$label: not accepted"; sed 's/^/    /' <<< "$out"; }
    else
        if grep -q '^  FAIL    .* build output(s) under samples/ or tests/' <<< "$out" && { [ -z "$name" ] || grep -qF "$name" <<< "$out"; }; then
            ok "$label: refused${name:+, naming $name}"
        else
            fail "$label: not refused as expected"; sed 's/^/    /' <<< "$out"
        fi
    fi
}
# tree <dir>: a minimal source-only release tree.
tree() {
    mkdir -p "$1/samples/lv2/hello/source" "$1/tests/regression/spurs-suite/row"
    printf 'cmake_minimum_required(VERSION 3.20)\n' > "$1/samples/lv2/hello/CMakeLists.txt"
    printf 'int main(void) { return 0; }\n' > "$1/samples/lv2/hello/source/main.c"
    printf 'int main(void) { return 0; }\n' > "$1/tests/regression/spurs-suite/row/main.c"
}

[ -f "$crt" ] || { echo "release-tree-build-outputs: no $crt"; exit 1; }
tree "$work/clean";  expect "clean sources" "$work/clean" ok
tree "$work/self";   : > "$work/self/samples/lv2/hello/hello.fake.self"
expect "a .fake.self beside a sample" "$work/self" fail "samples/lv2/hello/hello.fake.self"
tree "$work/elf";    : > "$work/elf/tests/regression/spurs-suite/row/row.elf"
expect "an .elf under tests/" "$work/elf" fail "tests/regression/spurs-suite/row/row.elf"
tree "$work/sprx";   : > "$work/sprx/samples/lv2/hello/hello.sprx"; : > "$work/sprx/samples/lv2/hello/hello.prx"
expect "a .sprx and .prx beside a sample" "$work/sprx" fail "samples/lv2/hello/hello.sprx"
tree "$work/cmake";  mkdir -p "$work/cmake/samples/lv2/hello/build/CMakeFiles"
: > "$work/cmake/samples/lv2/hello/build/CMakeCache.txt"; : > "$work/cmake/samples/lv2/hello/build/build.ninja"
expect "a CMake build dir in a sample" "$work/cmake" fail "samples/lv2/hello/build/CMakeCache.txt"
mkdir -p "$work/none"; expect "no samples/ or tests/" "$work/none" ok

[ $status -eq 0 ] && echo "PASS: release-tree-build-outputs-test" || echo "FAIL: release-tree-build-outputs-test"
exit $status
