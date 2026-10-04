#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
python3 "$here/glsl_types_check.py" "$1"
python3 "$here/glsl_types_reachability_check.py" "$1"
