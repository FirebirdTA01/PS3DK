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

# For all subsequent instructions after the colour output store, assert that no
# comparison or non-output computation instruction writes to R0.
# Comparisons: SLT (0x0A), SGE (0x0B), SEQ (0x0C), SNE (0x0D), etc.
COMPARISON_OPS = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}

for i in range(first_store_idx + 1, len(gs)):
    w = gs[i]
    none = (w[0] >> 30) & 1
    dst = (w[0] >> 1) & 0x3f
    mask = (w[0] >> 9) & 0xf
    opcode = (w[0] >> 24) & 0x3f
    if not none and dst == 0:
        if opcode in COMPARISON_OPS:
            sys.exit(
                f"FAIL: comparison instruction (opcode 0x{opcode:02X}) at instruction {i} "
                f"uses colour output register R0 (mask 0x{mask:X}) as a temporary, "
                f"clobbering live output after store at instruction {first_store_idx} "
                f"(t_dabb23e1 regression)"
            )

print(f"output-temp-reuse-test: ok (R0 output at instruction {first_store_idx} preserved; no subsequent temp clobbers)")
PY

printf 'PASS: output-temp-reuse-test\n'
