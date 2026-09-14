#!/usr/bin/env bash
# t_25fa9e31: check real containers, then every accepted tracked shader.
# Known findings are exact, carded entries; SDK evidence stays local-only.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" && -x "$compiler" ]] || { echo 'FAIL: compiler executable required' >&2; exit 1; }
PYTHON_BIN="${PYTHON:-python3}"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-uniform-crosscheck.XXXXXX")"
trap 'rm -rf "$work"' EXIT
"$PYTHON_BIN" "$repo_root/tests/shader-compiler/uniform_container_check_selftest.py" "$compiler"
"$PYTHON_BIN" "$repo_root/tests/shader-compiler/uniform_container_census.py" "$compiler" \
    --output "$work/census" \
    --allowlist "$repo_root/tests/shader-compiler/uniform_container_allowlist.json"
