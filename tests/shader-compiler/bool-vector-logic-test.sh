#!/usr/bin/env bash
# Component-wise !, && and || on vectors, judged by value (t_19e8402f).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/bool_vector_logic_check.py" "${1:?compiler required}"
