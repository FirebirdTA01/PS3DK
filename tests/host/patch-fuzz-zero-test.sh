#!/usr/bin/env bash
# Every patch application in the build and its tests uses --fuzz=0: a patch
# whose context no longer matches its target fails instead of landing wherever
# GNU patch's default fuzz (2 lines) lets it.  The series were refreshed so
# each applies exactly (the patched trees are unchanged); from then on,
# upstream or series drift is an error.
#
#  1. static, discovered recursively: every patch(1) invocation in any .sh
#     file under scripts/ and tests/ (comments and this file excluded) passes
#     --fuzz=0, and at least the six known sites exist (PPU, PPU kernel, SPU,
#     PSL1GHT, the libzip portlib recipe, the newlib-remove test);
#  2. control for the discovery: a planted tree with a NESTED recipe
#     (scripts/portlibs/999-planted.sh) that calls patch without --fuzz=0
#     must be reported, and the same tree with --fuzz=0 must be clean;
#  3. behaviour, with the patch(1) on PATH:
#       an exact patch applies with --fuzz=0;
#       a context-drift mutant (one context line changed in the target) is
#       REJECTED with --fuzz=0 and leaves the file unmodified ...
#       ... and applies with patch's default fuzz, which proves the mutant is
#       a real drift that the old invocations would have let through.
#
# usage: patch-fuzz-zero-test.sh
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
self="tests/host/patch-fuzz-zero-test.sh"
status=0
fail() { echo "patch-fuzz-zero: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# scan <root>: print "file:line: text" for every patch(1) invocation under
# <root>/scripts and <root>/tests (.sh files, any depth), prefixed SITE (has
# --fuzz=0) or BAD (does not).
scan() {
    local r=$1
    find "$r/scripts" "$r/tests" -type f -name '*.sh' 2>/dev/null | sort | while read -r f; do
        rel=${f#"$r"/}
        [ "$rel" = "$self" ] && continue
        awk -v rel="$rel" '
            { line = $0; sub(/^[[:space:]]+/, "", line) }
            line ~ /^#/ { next }
            line ~ /(^|[^[:alnum:]_.\/-])patch[[:space:]]+-/ {
                print ((line ~ /--fuzz=0/) ? "SITE " : "BAD ") rel ":" NR ": " line
            }' "$f"
    done
}

# 1. static
scan "$root" > "$work/scan.txt"
nsite=$(grep -c '^SITE ' "$work/scan.txt")
nbad=$(grep -c '^BAD ' "$work/scan.txt")
if [ "$nbad" -ne 0 ]; then fail "patch(1) without --fuzz=0:"; grep '^BAD ' "$work/scan.txt" | sed 's/^BAD /    /'; fi
if [ "$nsite" -lt 6 ]; then fail "found $nsite guarded patch(1) site(s), expected at least 6"; sed 's/^/    /' "$work/scan.txt"; fi
if [ "$nbad" -eq 0 ] && [ "$nsite" -ge 6 ]; then
    echo "patch-fuzz-zero: ok   all $nsite patch(1) sites under scripts/ and tests/ pass --fuzz=0"
    sed 's/^SITE /    /' "$work/scan.txt"
fi

# 2. control for the discovery: a nested, unguarded recipe must be reported
mkdir -p "$work/planted/scripts/portlibs" "$work/planted/tests/host"
printf '#!/usr/bin/env bash\n# patch -p1 in a comment is not a site\nfor p in x/*.patch; do\n    patch -p1 --forward --silent < "$p"\ndone\n' \
    > "$work/planted/scripts/portlibs/999-planted.sh"
scan "$work/planted" > "$work/planted.txt"
if [ "$(grep -c '^BAD scripts/portlibs/999-planted.sh:4:' "$work/planted.txt")" -eq 1 ] && [ "$(wc -l < "$work/planted.txt")" -eq 1 ]; then
    echo "patch-fuzz-zero: ok   control: an unguarded nested recipe is reported (and its comment is not)"
else
    fail "control: the planted nested recipe was not reported exactly once"; sed 's/^/    /' "$work/planted.txt"
fi
sed -i 's/--forward --silent/--forward --fuzz=0 --silent/' "$work/planted/scripts/portlibs/999-planted.sh"
scan "$work/planted" > "$work/planted2.txt"
if [ "$(grep -c '^SITE ' "$work/planted2.txt")" -eq 1 ] && [ "$(grep -c '^BAD ' "$work/planted2.txt")" -eq 0 ]; then
    echo "patch-fuzz-zero: ok   control: the same recipe with --fuzz=0 is clean"
else
    fail "control: the guarded planted recipe was not seen as a clean site"; sed 's/^/    /' "$work/planted2.txt"
fi

# 3. behaviour
command -v patch > /dev/null || { echo "patch-fuzz-zero: FAIL no patch(1) on PATH"; exit 1; }
mk() {  # mk <dir> <line 3 text>: a ten-line file whose third line varies
    mkdir -p "$1"
    { for i in 1 2; do echo "line $i"; done; echo "$2"; for i in 4 5 6 7 8 9 10; do echo "line $i"; done; } > "$1/f.txt"
}
mk "$work/exact" "line 3"
mk "$work/drift" "line 3 (changed upstream)"
mk "$work/fuzzy" "line 3 (changed upstream)"
cat > "$work/p.patch" <<'EOF'
--- a/f.txt
+++ b/f.txt
@@ -3,6 +3,7 @@
 line 3
 line 4
 line 5
+inserted
 line 6
 line 7
 line 8
EOF
if (cd "$work/exact" && patch -p1 --forward --fuzz=0 -s) < "$work/p.patch" > "$work/exact.out" 2>&1 \
   && grep -qx inserted "$work/exact/f.txt"; then
    echo "patch-fuzz-zero: ok   an exact patch applies with --fuzz=0"
else
    fail "an exact patch did not apply with --fuzz=0"; sed 's/^/    /' "$work/exact.out"
fi
if (cd "$work/drift" && patch -p1 --forward --fuzz=0 -s) < "$work/p.patch" > "$work/drift.out" 2>&1; then
    fail "the context-drift mutant applied with --fuzz=0"
elif grep -qx inserted "$work/drift/f.txt"; then
    fail "the rejected drift mutant still modified the file"
else
    echo "patch-fuzz-zero: ok   a context-drift mutant is rejected with --fuzz=0"
fi
if (cd "$work/fuzzy" && patch -p1 --forward -s) < "$work/p.patch" > "$work/fuzzy.out" 2>&1 \
   && grep -qx inserted "$work/fuzzy/f.txt"; then
    echo "patch-fuzz-zero: ok   control: the same mutant applies with patch's default fuzz"
else
    fail "control: the mutant did not apply with default fuzz, so it does not exercise fuzz"; sed 's/^/    /' "$work/fuzzy.out"
fi

[ $status -eq 0 ] && echo "PASS: patch-fuzz-zero-test"
exit $status
