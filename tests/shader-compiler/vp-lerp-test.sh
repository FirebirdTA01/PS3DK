#!/usr/bin/env bash
# Delta-order endpoints, extrapolation, modifiers and named contraction debt.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/vp_lerp_check.py" "${1:?compiler required}"
