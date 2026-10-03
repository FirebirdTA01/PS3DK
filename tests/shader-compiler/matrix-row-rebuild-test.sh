#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
python3 "$root/tests/shader-compiler/matrix_row_rebuild_check.py" "${1:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}"
