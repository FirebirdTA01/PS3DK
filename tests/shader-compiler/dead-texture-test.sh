#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$here/../../tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || { echo "FAIL: compiler not executable: $compiler" >&2; exit 1; }
python3 "$here/dead-texture-check.py" "$compiler"
