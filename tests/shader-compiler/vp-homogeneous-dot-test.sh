#!/usr/bin/env bash
# Homogeneous dot products and direct output writes in vertex programs; rows judged by value and shape.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/vp_homogeneous_dot_check.py" "${1:?compiler required}"
