#!/usr/bin/env bash
# Loop carries written in place: no copy per iteration, judged by value and shape.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/loop_carry_in_place_check.py" "${1:?compiler required}"
