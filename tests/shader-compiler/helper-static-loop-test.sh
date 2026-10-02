#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" && -x "$compiler" ]] || { echo 'usage: helper-static-loop-test.sh <compiler>' >&2; exit 1; }
"${PYTHON:-python3}" "$repo_root/tests/shader-compiler/helper_static_loop_check.py" "$compiler"
