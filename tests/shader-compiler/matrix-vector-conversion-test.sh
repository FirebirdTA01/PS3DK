#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
python3 "$root/tests/shader-compiler/matrix_vector_conversion_check.py" "$1"
