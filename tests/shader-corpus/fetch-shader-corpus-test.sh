#!/usr/bin/env bash
# Run this from a normal checkout, not a worktree under build/: the
# --check-root case rejects an in-repo destination outside
# build/shader-corpus, and a worktree whose own path contains build/
# makes that guard mis-fire on a path that only looks in-tree.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
script="$repo_root/scripts/fetch-shader-corpus.sh"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

csv="$("$script" --list)"

header="$(printf '%s\n' "$csv" | sed -n '1p')"
[[ "$header" == "id,name,repo,ref,path,license,usage,tracked_files,distinct_blobs,cg,vcg,fcg,notes" ]] \
    || fail "unexpected CSV header: $header"

row_for() {
    local id="$1"
    printf '%s\n' "$csv" | awk -F, -v id="$id" '$1 == id { print }'
}

assert_row() {
    local id="$1" usage="$2" tracked="$3" distinct="$4" cg="$5" vcg="$6" fcg="$7"
    local row
    row="$(row_for "$id")"
    [[ -n "$row" ]] || fail "missing row for $id"

    IFS=, read -r rid _name _repo _ref _path _license rusage rtracked rdistinct rcg rvcg rfcg _notes <<<"$row"
    [[ "$rid" == "$id" ]] || fail "$id row id mismatch"
    [[ "$rusage" == "$usage" ]] || fail "$id usage: got $rusage want $usage"
    [[ "$rtracked" == "$tracked" ]] || fail "$id tracked count: got $rtracked want $tracked"
    [[ "$rdistinct" == "$distinct" ]] || fail "$id distinct count: got $rdistinct want $distinct"
    [[ "$rcg" == "$cg" ]] || fail "$id .cg count: got $rcg want $cg"
    [[ "$rvcg" == "$vcg" ]] || fail "$id .vcg count: got $rvcg want $vcg"
    [[ "$rfcg" == "$fcg" ]] || fail "$id .fcg count: got $rfcg want $fcg"
}

assert_row ps3-open-graphics-toolkit fetch-run-only 87 87 0 1 86
assert_row ioquake3-ps3 fetch-run-only 8 8 0 1 7
assert_row th06-ps3 fetch-run-only 11 11 0 1 10
assert_row classicube fetch-run-only 5 5 0 3 2
assert_row crystalct-psl1ght vendor-eligible 14 6 0 7 7
assert_row rsxgl vendor-eligible 10 10 0 5 5
assert_row libretro-common-shaders fetch-run-only 577 572 577 0 0
assert_row ogre-cg-samples vendor-eligible 41 41 41 0 0
assert_row ogre-examples excluded 4 3 4 0 0
assert_row shader-tut excluded 0 0 0 0 0

printf '%s\n' "$csv" | grep -qi 'sce-' && fail "public corpus metadata names private tool prefixes"

# The licence guard is an allowlist: a vendor-eligible source must carry a
# vendorable licence, so flipping a GPL source to vendor-eligible must be
# refused - a denylist would have passed a licence string it did not know.
guard_tmp="$(mktemp -d)"
trap 'rm -rf "$guard_tmp"' EXIT
sed 's/USAGE\[th06-ps3\]="fetch-run-only"/USAGE[th06-ps3]="vendor-eligible"/' "$script" > "$guard_tmp/gpl.sh"
if bash "$guard_tmp/gpl.sh" --dry-run >"$guard_tmp/gpl.out" 2>&1; then
    fail "a GPL source flipped to vendor-eligible was accepted - the licence guard is not an allowlist"
fi
grep -q 'not on the vendorable allowlist' "$guard_tmp/gpl.out" \
    || fail "the GPL-vendor refusal did not name the allowlist"

# A prefix glob would have admitted an ambiguous existing pin: classicube's
# licence string ends in NOASSERTION, so flipping it to vendor-eligible must
# be refused by an exact-match allowlist even though it starts with a BSD
# family name.
sed 's/USAGE\[classicube\]="fetch-run-only"/USAGE[classicube]="vendor-eligible"/' "$script" > "$guard_tmp/cc.sh"
if bash "$guard_tmp/cc.sh" --dry-run >"$guard_tmp/cc.out" 2>&1; then
    fail "an ambiguous BSD-*-NOASSERTION licence was admitted by a prefix match - the allowlist is not exact"
fi
grep -q 'not on the vendorable allowlist' "$guard_tmp/cc.out" \
    || fail "the ambiguous-licence refusal did not name the allowlist"

# An entirely unreviewed licence string is refused, not admitted by shape.
sed 's/LICENSE\[rsxgl\]="BSD-2-Clause-style"/LICENSE[rsxgl]="BSD-2-Clause-style-with-extra-unreviewed-terms"/' "$script" > "$guard_tmp/uk.sh"
if bash "$guard_tmp/uk.sh" --dry-run >"$guard_tmp/uk.out" 2>&1; then
    fail "an unreviewed licence suffix on a vendor-eligible source was accepted"
fi
grep -q 'not on the vendorable allowlist' "$guard_tmp/uk.out" \
    || fail "the unreviewed-licence refusal did not name the allowlist"

# An excluded source named explicitly is refused, not silently fetched.
if bash "$script" --source ogre-examples --root "$guard_tmp/x" >"$guard_tmp/exc.out" 2>&1; then
    fail "an excluded source was fetchable by explicit --source"
fi
grep -q 'is excluded' "$guard_tmp/exc.out" \
    || fail "the excluded --source refusal did not say the source is excluded"

# The exclusion must also fire under --dry-run: the question "what would
# this do?" must not report success for a source the script will not fetch.
if bash "$script" --dry-run --source ogre-examples >"$guard_tmp/dryexc.out" 2>&1; then
    fail "an excluded source reported ok under --dry-run --source"
fi
grep -q 'is excluded' "$guard_tmp/dryexc.out" \
    || fail "the excluded --dry-run refusal did not say the source is excluded"

"$script" --dry-run >/tmp/fetch-shader-corpus-dry-run.out
grep -q '^metadata ok: 10 sources, 757 tracked files, 743 distinct shader blobs$' \
    /tmp/fetch-shader-corpus-dry-run.out \
    || fail "unexpected dry-run summary"

bad_root="$repo_root/docs/build/shader-corpus"
if "$script" --check-root "$bad_root" >/tmp/fetch-shader-corpus-bad-root.out 2>&1; then
    fail "accepted in-repo root outside build/: $bad_root"
fi
grep -q 'destination is not gitignored - refusing to fetch licensed sources into a trackable path' \
    /tmp/fetch-shader-corpus-bad-root.out \
    || fail "bad-root failure did not explain gitignored licensing requirement"
[[ ! -e "$bad_root" ]] || fail "bad-root guard created rejected destination: $bad_root"

"$script" --check-root "$repo_root/build/shader-corpus" >/tmp/fetch-shader-corpus-good-root.out
grep -q 'destination ok:' /tmp/fetch-shader-corpus-good-root.out \
    || fail "ignored in-repo root was not accepted"

"$script" --check-root /tmp/ps3dk-shader-corpus-root-check >/tmp/fetch-shader-corpus-outside-root.out
grep -q 'destination ok:' /tmp/fetch-shader-corpus-outside-root.out \
    || fail "outside-repo root was not accepted"

printf 'fetch-shader-corpus-test: ok\n'
