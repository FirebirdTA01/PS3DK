#!/usr/bin/env bash
# Const helper inputs reject writes while mutable copies and shadows stay legal.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$here/const_parameter_check.py" "${1:?compiler required}"
