#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
compiler="${1:?usage: vp-normalize-width-test.sh compiler}"
exec "${PYTHON:-python3}" "$here/vp_normalize_width_check.py" "$compiler"
