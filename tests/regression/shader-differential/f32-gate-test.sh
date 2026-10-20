#!/usr/bin/env bash
# Builds and runs the host test of the float-mode control gate
# (source/f32_gate.h, used by the guest's run_f32) and the host float
# comparator's self-test.  Needs a host C compiler (cc, or $CC).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$here" -o "$work/f32-gate-test" "$here/f32-gate-test.c"
"$work/f32-gate-test"
python3 "$here/f32_compare.py" --self-test
# the pinned matrix recipe table equals the one computed from the auto-matrix rule
python3 "$here/f32_matrix_controls.py" --check "$here/source/f32_gate.h"
