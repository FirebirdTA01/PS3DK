#!/usr/bin/env bash
# Fragment inputs with no semantic, and uniform members of a varying struct.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/implicit_varying_check.py" "${1:?compiler required}"
