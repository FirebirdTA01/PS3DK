#!/usr/bin/env bash
# computed-lane-insert: Non-contiguous lane-preserving insert of a computed vector must compile
# on the general path and write all four destination channels into R0 (.yw preserved,
# .xz inserted from computed temp).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-computed-lane-insert-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
src="$shaders/fp_computed_lane_insert_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

# 1. Shipping path (general lowering)
# The ucode dump is on stdout and the diagnostics are on stderr; merging
# them lets a stderr line land INSIDE a hex row, which costs the row,
# shifts every later one and decodes a constant as an instruction writing
# a register nothing reads (the false R33, 2026-09-07).  Keep them apart;
# the decoder's refusal is the fallback, not the fix.
log_gen="$work/computed_lane_insert_general.log"
err_gen="$work/computed_lane_insert_general.err"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$log_gen" 2>"$err_gen" || {
    tail -n 30 "$err_gen" >&2
    fail "fp_computed_lane_insert_f.cg did not compile on shipping path"
}

python3 - "$repo_root/tests/shader-compiler" "$log_gen" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from ucode_decode import groups

gs = groups(sys.argv[2])
if not gs:
    sys.exit("FAIL: no ucode groups decoded from general path log")

# Accumulate all write masks written to R0 (dst == 0, none == 0)
combined_mask = 0
has_x_insert = False
has_z_insert = False

for i, w in enumerate(gs):
    none = (w[0] >> 30) & 1
    dst = (w[0] >> 1) & 0x3f
    mask = (w[0] >> 9) & 0xf
    if not none and dst == 0:
        combined_mask |= mask
        if mask & 0x1:
            has_x_insert = True
        if mask & 0x4:
            has_z_insert = True

if combined_mask != 0xF:
    sys.exit(
        f"FAIL: R0 channels not fully written: combined mask 0x{combined_mask:X} != 0xF "
        f"(expected all 4 channels .xyzw; computed-lane-insert regression)"
    )

if not has_x_insert or not has_z_insert:
    sys.exit("FAIL: non-contiguous lane inserts for .x (bit 0) and .z (bit 2) not present in ucode")

print(f"computed-lane-insert-test: ok (general path writes all channels 0x{combined_mask:X} with non-contiguous .x/.z inserts)")
PY

# legacy-path control removed with the shape matcher (chore/rsxcg-remove-legacy-lowering); it proved the fixture is a non-literal VecInsert the matcher refused (literal-lane-insert).

printf 'PASS: computed-lane-insert-test\n'
