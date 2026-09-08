#!/usr/bin/env bash
# Explicit sampler units must agree in reflection and the actual texture word.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -x "$compiler" ]] || { echo 'FAIL: executable compiler required' >&2; exit 1; }
exec "${PYTHON:-python3}" "$here/sampler_binding_check.py" "$compiler"
