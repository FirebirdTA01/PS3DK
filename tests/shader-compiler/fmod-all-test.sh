#!/usr/bin/env bash
# fmod() and all() are lowered, not left as calls.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/fmod_all_check.py" "${1:?compiler required}"
