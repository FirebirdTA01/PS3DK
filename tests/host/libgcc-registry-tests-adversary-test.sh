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

# run <test> <env...>: the test's exit status.  Its complete raw output is kept in THIS test's log (indented, framed
# by the case), so the site each adversary was caught at survives the temporary directory (review: codex).
run() {
    local t=$1 rc; shift
    env "$@" bash "$here/$t.sh" > "$work/out" 2>&1; rc=$?
    { printf '    --- %s %s (exit %d)\n' "$t" "$*" "$rc"; sed 's/^/    | /' "$work/out"; } >&2
    echo $rc
}

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
    if [ $t = libgcc-heap-ready-link-test ]; then
        # Selective: nm is truthful for the libgcc archive (so the test gets past its archive inspection) and lies
        # only for the LINKED program, the stage.elf check (review: codex - the generic liar never reached it).
        printf '#!/bin/sh\nfor a; do last=$a; done\n"%s" "$@"; rc=$?\ncase $last in *.elf) exit 1 ;; esac\nexit $rc\n' "$P-nm" \
            > "$work/nm-elf-liar" && chmod +x "$work/nm-elf-liar" || { echo "cannot write nm-elf-liar"; exit 1; }
        rc=$(run $t "NM=$work/nm-elf-liar")
        [ "$rc" = 1 ] && echo "libgcc-registry-adversary: ok $t fails when nm lies only for the linked program" \
                      || fail "$t exited $rc when nm lies only for the linked program (want 1)"
        # realpath that prints nothing and fails, and one that prints the real path and then fails: the -B route
        # must never be declared proven by two empty (or unchecked) canonical paths.
        printf '#!/bin/sh\nexit 1\n' > "$work/realpath-silent" && chmod +x "$work/realpath-silent" || exit 1
        for rp in "$work/realpath-silent" "$(liar realpath-liar "$(command -v realpath)")"; do
            rc=$(run $t "REALPATH=$rp")
            [ "$rc" = 1 ] && echo "libgcc-registry-adversary: ok $t fails when realpath fails (${rp##*/})" \
                          || fail "$t exited $rc when realpath fails (${rp##*/}, want 1)"
        done
    fi
done
[ $status -eq 0 ] && echo "PASS: libgcc-registry-tests-adversary-test"
exit $status
