#!/usr/bin/env bash
# MRT outputs occupy R0/R2/R3/R4 (or H0/H4/H6/H8); DEPTH remains R1.z.
# Mapping all colours to R0, dropping the half-bank flag, or excluding
# outputs from registerCount must fail this guard (t_cfff9343).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$here/../../tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
[[ -x "$compiler" ]] || { printf 'FAIL: compiler not executable: %s\n' "$compiler" >&2; exit 1; }
python3 "$here/mrt_output_check.py" "$compiler"
