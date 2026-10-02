#!/usr/bin/env bash
# A helper whose returns sit under compile-time-constant conditions (t_a290c3c8).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/helper_const_return_check.py" "${1:?compiler required}"
