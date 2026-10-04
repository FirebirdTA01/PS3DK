#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
python3 "$here/glsl_functions_check.py" "${1:?compiler path required}"
