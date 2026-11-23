#!/usr/bin/env bash
# Lane-by-lane copies of one source are one MOV in fragment programs; rows judged by value and shape.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/fp_lane_move_check.py" "${1:?compiler required}"
