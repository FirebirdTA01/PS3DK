#!/usr/bin/env bash
# Scalars share registers when a fragment program is over the register budget; judged by value.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/fp_scalar_pack_check.py" "${1:?compiler required}"
