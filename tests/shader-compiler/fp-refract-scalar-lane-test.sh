#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
python3 "$root/tests/shader-compiler/fp_refract_scalar_lane_check.py" "${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
