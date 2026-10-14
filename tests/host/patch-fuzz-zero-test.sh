#!/usr/bin/env bash
# Every patch series is applied with --fuzz=0: a patch whose context no longer
# matches its target fails the build instead of landing wherever GNU patch's
# default fuzz (2 lines) lets it.  The series were refreshed so each applies
# exactly (the patched trees are unchanged); from then on, upstream or series
# drift is an error.
#
#  1. static: every `patch -p1` call in scripts/*.sh passes --fuzz=0, and at
#     least the four known apply sites exist (PPU, PPU kernel, SPU, PSL1GHT);
#  2. behaviour, with the patch(1) on PATH:
#       an exact patch applies with --fuzz=0;
#       a context-drift mutant (one context line changed in the target) is
#       REJECTED with --fuzz=0 ...
#       ... and accepted with patch's default fuzz, which proves the mutant is a
#       real drift that the old invocation would have let through.
#
# usage: patch-fuzz-zero-test.sh
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
status=0
fail() { echo "patch-fuzz-zero: FAIL $*"; status=1; }

# 1. static
calls=$(grep -n 'patch -p1' "$root"/scripts/*.sh)
n=$(printf '%s\n' "$calls" | grep -c 'patch -p1')
bad=$(printf '%s\n' "$calls" | grep -v -- '--fuzz=0')
if [ "$n" -lt 4 ]; then fail "found $n patch -p1 call(s) in scripts/, expected at least 4"; fi
if [ -n "$bad" ]; then fail "patch -p1 without --fuzz=0:"; printf '    %s\n' "$bad"; fi
[ "$n" -ge 4 ] && [ -z "$bad" ] && echo "patch-fuzz-zero: ok   all $n patch -p1 calls in scripts/ pass --fuzz=0"

# 2. behaviour
command -v patch > /dev/null || { echo "patch-fuzz-zero: FAIL no patch(1) on PATH"; exit 1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
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
