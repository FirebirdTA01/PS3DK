#!/usr/bin/env bash
# local-array-index-test.sh - local fixed array dynamic index refusal.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

# A refusal is exit 1 EXACTLY.  124 is a timeout and >= 128 is a signal, and
# either one satisfies "did not exit 0" while meaning the compiler never
# reached the decision this guard is about - so a compiler that CRASHED on a
# shader it should have refused BY NAME was reported as correct here.  Call
# this wherever a compile's status is captured, whichever way that compile is
# expected to go: it is silent for 0 and for 1 and names anything else.
# Measured: half the guards in this suite that assert a refusal could not tell
# one from a SIGABRT (t_fd95d1b9).
refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_local_array_dynamic_index_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-local-array-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

rc=0
"$compiler" -p sce_fp_rsx --emit-container "$work/out.fpo" "$src" \
    >"$work/general.log" 2>&1 || rc=$?
refusal_status "$rc" "fp_local_array_dynamic_index_f"
[[ "$rc" -eq 1 ]] || fail "dynamic local array index compiled; sce_fp_rsx reference rejects this profile-restricted shape"

grep -q "local array dynamic indexing is not supported" "$work/general.log" || {
    tail -n 20 "$work/general.log" >&2
    fail "dynamic local array index refused with an unexpected diagnostic"
}

printf 'local-array-index-test: ok\n'
