#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/mutable_static_array_index_check.py" "${1:?compiler required}"
