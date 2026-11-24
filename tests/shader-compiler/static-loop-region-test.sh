#!/usr/bin/env bash
# Large fixed-count fragment loops kept as hardware loops, judged by value and shape.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/static_loop_region_check.py" "${1:?compiler required}"
