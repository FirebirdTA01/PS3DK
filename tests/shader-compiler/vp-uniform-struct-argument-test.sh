#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/vp_uniform_struct_argument_check.py" "${1:?compiler required}" "${@:2}"
