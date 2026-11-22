#!/usr/bin/env bash
# A struct passed by value to a helper is the argument's value, judged by value.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/struct_value_argument_check.py" "${1:?compiler required}"
