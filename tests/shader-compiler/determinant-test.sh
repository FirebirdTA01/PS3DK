#!/bin/sh
set -eu
COMPILER=${1:-tools/rsx-cg-compiler/build/rsx-cg-compiler}
python3 "$(dirname "$0")/determinant_check.py" "$COMPILER"
