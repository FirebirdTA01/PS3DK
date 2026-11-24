#!/usr/bin/env bash
# Fragment programs carry no register self-copies; lerp rows judged by value and shape.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/fp_self_copy_check.py" "${1:?compiler required}"
