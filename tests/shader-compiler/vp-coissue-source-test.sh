#!/usr/bin/env bash
# Value regression for a vector operand overwritten by a scalar co-issue.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/vp_coissue_source_check.py" "${1:?compiler required}"
