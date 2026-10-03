#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
python3 "$repo_root/tests/shader-compiler/fp_matrix_addsub_check.py" "$compiler"
