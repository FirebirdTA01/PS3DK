#!/usr/bin/env bash
# Named helper static parameter extension retains const and ordinary body checks.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$here/static_parameter_check.py" "${1:?compiler required}"
