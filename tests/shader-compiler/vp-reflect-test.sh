#!/usr/bin/env bash
# Evaluate every reflected lane, including non-unit normals and source modifiers.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/vp_reflect_check.py" "${1:?compiler required}"
