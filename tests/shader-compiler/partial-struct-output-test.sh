#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$here/../../tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 "$here/partial_struct_output_check.py" "$compiler"
