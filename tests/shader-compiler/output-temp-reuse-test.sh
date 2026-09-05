#!/usr/bin/env bash
# t_dabb23e1: The colour output register R0 must stay live across the entire program
# after an early store, and must not be allocated as a scratch temporary for
# intermediate calculations (such as comparisons or conditions).
#
# Measured on fp_discard_nested_f.cg:
#   if (c.a < 0.5) { if (d.a < 0.5) discard; } o = c;
# Under the defect, the output store to R0 was scheduled early, and the allocator
# treated R0 as dead after the store, handing R0.x to the outer comparison SLTR,
# clobbering the colour output with the comparison result.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-output-temp-reuse-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
src="$shaders/fp_discard_nested_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

log="$work/fp_discard_nested.log"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "fp_discard_nested_f.cg did not compile"
}

python3 - "$repo_root/tests/shader-compiler" "$log" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from ucode_decode import groups

gs = groups(sys.argv[2])
if not gs:
    sys.exit("FAIL: no ucode groups decoded from log")

# Locate the initial colour output store writing to R0
first_store_idx = None
for i, w in enumerate(gs):
    none = (w[0] >> 30) & 1
    dst = (w[0] >> 1) & 0x3f
    mask = (w[0] >> 9) & 0xf
    opcode = (w[0] >> 24) & 0x3f
    # In fp_discard_nested_f, o = c writes full mask 0xF to R0 via MOV (0x01)
    if not none and dst == 0 and opcode == 0x01:
        first_store_idx = i
        break

if first_store_idx is None:
    sys.exit("FAIL: could not find colour output store to R0")

# In fp_discard_nested_f, there is exactly one output store (o = c).
# The store writes R0 (mask 0xF). Because this shader contains no subsequent
# output assignments, no instruction after first_store_idx is permitted to write
# any lane that the store wrote. Any subsequent write to R0 (regardless of opcode —
# whether comparison SLT, arithmetic ADD/MUL/MAD, etc.) represents the allocator
# reusing live colour output R0 as a scratch temporary (t_dabb23e1).
store_mask = (gs[first_store_idx][0] >> 9) & 0xf

for i in range(first_store_idx + 1, len(gs)):
    w = gs[i]
    none = (w[0] >> 30) & 1
    dst = (w[0] >> 1) & 0x3f
    mask = (w[0] >> 9) & 0xf
    opcode = (w[0] >> 24) & 0x3f
    if not none and dst == 0 and (mask & store_mask) != 0:
        sys.exit(
            f"FAIL: instruction at {i} (opcode 0x{opcode:02X}) writes to colour output "
            f"register R0 (mask 0x{mask:X}, overlapping store mask 0x{store_mask:X}), "
            f"clobbering live colour output after store at instruction {first_store_idx} "
            f"(t_dabb23e1 regression)"
        )

print(f"output-temp-reuse-test: ok (R0 output at instruction {first_store_idx} preserved; no subsequent temp clobbers)")
PY

printf 'PASS: output-temp-reuse-test\n'
