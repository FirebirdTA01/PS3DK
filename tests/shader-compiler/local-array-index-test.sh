#!/usr/bin/env bash
# local-array-index-test.sh - local fixed array dynamic index refusal.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_local_array_dynamic_index_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-local-array-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

if "$compiler" -p sce_fp_rsx --emit-container "$work/out.fpo" "$src" \
    >"$work/general.log" 2>&1; then
    fail "dynamic local array index compiled; sce_fp_rsx reference rejects this profile-restricted shape"
fi

grep -q "local array dynamic indexing is not supported" "$work/general.log" || {
    tail -n 20 "$work/general.log" >&2
    fail "dynamic local array index refused with an unexpected diagnostic"
}

printf 'local-array-index-test: ok\n'
