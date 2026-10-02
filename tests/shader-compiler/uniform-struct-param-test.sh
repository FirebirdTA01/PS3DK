#!/usr/bin/env bash
# A uniform struct entry parameter is one uniform per member (t_31ed8939).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/uniform_struct_param_check.py" "${1:?compiler required}"
