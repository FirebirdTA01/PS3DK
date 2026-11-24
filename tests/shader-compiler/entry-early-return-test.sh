#!/usr/bin/env bash
# A fragment entry may return from inside an if; rows judged by value.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/entry_early_return_check.py" "${1:?compiler required}"
