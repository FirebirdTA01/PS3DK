#!/usr/bin/env bash
# t_74f97caa: FP tex1D coordinate packing and sampler reflection.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -x "$compiler" ]] || { echo 'FAIL: an executable compiler is required' >&2; exit 1; }
exec "${PYTHON:-python3}" "$here/tex1d_check.py" "$compiler"
