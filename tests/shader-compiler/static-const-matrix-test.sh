#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/static_const_matrix_check.py" "${1:?compiler required}"
