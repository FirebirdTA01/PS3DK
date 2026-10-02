#!/usr/bin/env bash
# Programs the reference refuses must refuse here too (t_fff5cf2a).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/wrong_accept_check.py" "${1:?compiler required}"
