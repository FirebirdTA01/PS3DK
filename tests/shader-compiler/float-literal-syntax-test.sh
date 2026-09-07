#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$here/../../tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
python3 "$here/float_literal_syntax_check.py" "$compiler"
