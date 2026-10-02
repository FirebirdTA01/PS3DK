#!/usr/bin/env bash
# A helper's out / inout parameters are copy-out (t_a290c3c8 follow-up).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/inline_out_param_check.py" "${1:?compiler required}"
