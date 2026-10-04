#!/bin/sh
set -eu
COMPILER=${1:-tools/rsx-cg-compiler/build/rsx-cg-compiler}
python3 "$(dirname "$0")/matrix_folded_constant_check.py" "$COMPILER"
