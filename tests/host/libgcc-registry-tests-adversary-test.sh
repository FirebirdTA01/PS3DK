#!/usr/bin/env bash
# The libgcc registry host tests (libgcc-fde-fast-path-test, libgcc-heap-ready-link-test) must FAIL when a tool
# prints valid-looking output and then exits non-zero.  Each tool the tests read is replaced in turn by a wrapper
# that runs the real tool, prints its output, and exits 1; the whole test must exit 1.  A control run with the
# real tools must pass, so a failure here is the tool's, not the stage's.  Skips without a PS3DEV stage that has
# the 0074 registry (as the two tests themselves do).
set -u -o pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
P=${PS3DEV:-}/ppu/bin/powerpc64-ps3-elf
if [ -z "${PS3DEV:-}" ] || [ ! -x "$P-gcc" ]; then echo "SKIP: libgcc-registry-tests-adversary-test (no PS3DEV compiler)"; exit 0; fi
work=$(mktemp -d) || { echo "libgcc-registry-adversary: FAIL mktemp"; exit 1; }
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "libgcc-registry-adversary: FAIL $*"; status=1; }

# liar <name> <real tool>: a wrapper that runs the real tool with its arguments, keeps its output, exits 1
liar() {
    printf '#!/bin/sh\n"%s" "$@"\nexit 1\n' "$2" > "$work/$1" && chmod +x "$work/$1" || { echo "cannot write $1"; exit 1; }
    echo "$work/$1"
}

# run <test> <env...>: the test's exit status
run() { local t=$1; shift; env "$@" bash "$here/$t.sh" > "$work/out" 2>&1; echo $?; }

for t in libgcc-fde-fast-path-test libgcc-heap-ready-link-test; do
    rc=$(run $t)
    if [ "$rc" != 0 ] || grep -q '^SKIP' "$work/out"; then
        echo "SKIP: libgcc-registry-tests-adversary-test ($t does not pass on this stage: rc $rc, $(head -n 1 "$work/out"))"
        [ "$rc" = 0 ] || status=1
        continue
    fi
    echo "libgcc-registry-adversary: ok $t passes with the real tools"
    case $t in
        libgcc-fde-fast-path-test) tools="AR:$P-ar NM:$P-nm OBJDUMP:$P-objdump AWK:$(command -v awk)" ;;
        libgcc-heap-ready-link-test) tools="NM:$P-nm OBJCOPY:$P-objcopy" ;;
    esac
    for tv in $tools; do
        var=${tv%%:*}; real=${tv#*:}
        rc=$(run $t "$var=$(liar "liar-$var" "$real")")
        [ "$rc" = 1 ] && echo "libgcc-registry-adversary: ok $t fails (exit 1) when $var prints its output and exits 1" \
                      || fail "$t exited $rc with a lying $var (want 1)"
    done
done
[ $status -eq 0 ] && echo "PASS: libgcc-registry-tests-adversary-test"
exit $status
