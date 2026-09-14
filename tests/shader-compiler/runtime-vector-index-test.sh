#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" && -x "$compiler" ]] || { echo 'usage: runtime-vector-index-test.sh <compiler>' >&2; exit 1; }
"${PYTHON:-python3}" "$repo_root/tests/shader-compiler/runtime_vector_index_check.py" "$compiler"
