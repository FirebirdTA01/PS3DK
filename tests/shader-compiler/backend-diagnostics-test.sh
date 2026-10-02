#!/usr/bin/env bash
# Backend refusals must lead with the construct that failed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/backend_diagnostics_check.py" "${1:?compiler required}"
