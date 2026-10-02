#!/usr/bin/env bash
# PSIZE/CLP must write their measured physical lanes and advertise them.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
[[ -x "$compiler" ]] || { echo "FAIL: compiler not executable: $compiler" >&2; exit 1; }
python3 "$root/tests/shader-compiler/vp_special_output_check.py" "$compiler"
