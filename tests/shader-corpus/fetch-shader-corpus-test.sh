#!/usr/bin/env bash
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

printf '%s\n' "$csv" | grep -qi 'sce-' && fail "public corpus metadata names private tool prefixes"

"$script" --dry-run >/tmp/fetch-shader-corpus-dry-run.out
grep -q '^metadata ok: 6 sources, 135 tracked files, 127 distinct shader blobs$' \
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
