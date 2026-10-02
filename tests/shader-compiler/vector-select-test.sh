#!/usr/bin/env bash
# Vector conditions in ?:, judged by value (t_3b3a3e1e).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/vector_select_check.py" "${1:?compiler required}"
