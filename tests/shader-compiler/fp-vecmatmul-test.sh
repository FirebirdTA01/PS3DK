#!/usr/bin/env bash
# FP row-vector products: measured MUL row 1, MAD row 0, then rows 2/3.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" && -x "$compiler" ]] || { echo 'usage: fp-vecmatmul-test.sh <compiler>' >&2; exit 1; }
"${PYTHON:-python3}" "$repo_root/tests/shader-compiler/fp_vecmatmul_check.py" "$compiler"
