#!/usr/bin/env bash
# check-release-tree.sh section 6: a release tree must ship samples/ and tests/ as source only.  Packaging from a
# tree in which samples or regression rows were built carries their .self/.elf/.sprx/.prx and CMake build files into
# the zip (a 2026-10-16 GCC 16 packaging dry run shipped 28 prebuilt .fake.self files).
#
# Runs check-release-tree.sh on small synthetic trees and reads only section 6's verdict (the other sections fail on
# a synthetic tree, as they should):
#   clean sources, and a tree with no samples/ or tests/   -> "ok      no .self/.elf/..."
#   one tree per marker - .self, .fake.self, .elf, .sprx, .prx, CMakeCache.txt, build.ninja, a CMakeFiles/ dir -
#   each alone                                            -> FAIL naming it
#   find failing on samples/ (exit 1, or exit 0 with a stderr complaint) with an output planted there
#                                                         -> FAIL "could not scan", never ok
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
tree "$work/clean"; expect "clean sources" "$work/clean" ok
mkdir -p "$work/none"; expect "no samples/ or tests/" "$work/none" ok
# One tree per marker, so each predicate is witnessed on its own (codex 8cd38b21: grouped cases let a dropped
# predicate pass).
n=0
for rel in samples/lv2/hello/hello.self samples/lv2/hello/hello.fake.self tests/regression/spurs-suite/row/row.elf \
           samples/lv2/hello/hello.sprx samples/lv2/hello/hello.prx samples/lv2/hello/build/CMakeCache.txt \
           samples/lv2/hello/build/build.ninja samples/lv2/hello/build/CMakeFiles; do
    n=$((n + 1)); t="$work/m$n"; tree "$t"; mkdir -p "$(dirname "$t/$rel")"
    if [[ $rel == */CMakeFiles ]]; then mkdir -p "$t/$rel"; else : > "$t/$rel"; fi
    expect "${rel##*/} alone" "$t" fail "$rel"
done
# A scan that fails must not read as clean: a find wrapped to fail (or only to complain) on samples/, with a planted
# output there, has to be refused as a scan failure, not accepted.
real_find=$(command -v find)
mkdir -p "$work/bin-exit" "$work/bin-warn"
printf '#!/usr/bin/env bash\ncase "$1" in */samples) echo "find: %s: Permission denied" "$1/hidden" >&2; exit 1;; esac\nexec "%s" "$@"\n' '%s' "$real_find" > "$work/bin-exit/find"
printf '#!/usr/bin/env bash\ncase "$1" in */samples) echo "find: %s: Permission denied" "$1/hidden" >&2; "%s" "$@"; exit 0;; esac\nexec "%s" "$@"\n' '%s' "$real_find" "$real_find" > "$work/bin-warn/find"
chmod +x "$work/bin-exit/find" "$work/bin-warn/find"
for kind in exit warn; do
    t="$work/scan-$kind"; tree "$t"; : > "$t/samples/hidden.fake.self"
    out=$(PATH="$work/bin-$kind:$PATH" section6 "$t")
    if grep -q '^  FAIL    could not scan samples/ and tests/ for build outputs' <<< "$out" &&
       ! grep -q '^  ok ' <<< "$out" && grep -q 'Permission denied' <<< "$out"; then
        ok "find failing on samples/ ($kind): refused as a scan failure"
    else
        fail "find failing on samples/ ($kind): not refused as a scan failure"; sed 's/^/    /' <<< "$out"
    fi
done

[ $status -eq 0 ] && echo "PASS: release-tree-build-outputs-test" || echo "FAIL: release-tree-build-outputs-test"
exit $status
