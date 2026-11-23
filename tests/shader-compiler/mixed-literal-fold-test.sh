#!/usr/bin/env bash
# Float and integer literals fold together in fragment programs; rows judged by value and shape.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$here/mixed_literal_fold_check.py" "${1:?compiler required}"
