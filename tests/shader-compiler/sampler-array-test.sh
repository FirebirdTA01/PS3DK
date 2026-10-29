#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -x "$compiler" ]] || { echo 'FAIL: executable compiler required' >&2; exit 1; }
exec "${PYTHON:-python3}" "$here/sampler_array_check.py" "$compiler"
