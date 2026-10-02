#!/usr/bin/env bash
# A vector stored into a scalar keeps lane x.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/scalar_narrowing_check.py" "${1:?compiler required}"
