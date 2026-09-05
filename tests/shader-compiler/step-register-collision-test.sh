#!/usr/bin/env bash
# t_929c0177: preferredPhys must yield when a physical register is already live;
# two simultaneously-live pinned results must not collide on the same register.
#
# Measured on test_62 pattern: `border = step(0.05, u) * step(u, 0.95);`
# Under the defect, lowerStep unconditionally pinned both step() results to phys 0,
# causing the consuming multiply to read R0 twice and compute a*a instead of a*b.
#
# NOTE: Passing this test proves absence of register collision between these two
# live values; it does not assert that the register allocator achieves
# reference-parity register efficiency.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-step-collision-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
src="$shaders/fp_step_collision_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

log="$work/fp_step_collision.log"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "fp_step_collision_f.cg did not compile"
}

python3 - "$repo_root/tests/shader-compiler" "$log" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from ucode_decode import groups

gs = groups(sys.argv[2])
if not gs:
    sys.exit("FAIL: no ucode groups decoded from log")

# Find the MUL instruction that computes a * b (opcode 0x02, destination temp)
mul_found = False
for i, w in enumerate(gs):
    opcode = (w[0] >> 24) & 0x3f
    dst = (w[0] >> 1) & 0x3f
    none = (w[0] >> 30) & 1
    if opcode == 0x02 and not none:
        src0_type = w[1] & 3
        src0_reg = (w[1] >> 2) & 0x3f
        src1_type = w[2] & 3
        src1_reg = (w[2] >> 2) & 0x3f
        # The multiply of the two step results reads two TEMP operands
        if src0_type == 0 and src1_type == 0:
            mul_found = True
            if src0_reg == src1_reg:
                sys.exit(
                    f"FAIL: step collision on physical register R{src0_reg}: "
                    f"consuming MUL at instruction {i} reads R{src0_reg} for both operands "
                    f"(computes a*a instead of a*b; t_929c0177 regression)"
                )
            print(f"step-register-collision-test: ok (MUL at {i} reads distinct registers R{src0_reg} and R{src1_reg})")
            break

if not mul_found:
    sys.exit("FAIL: could not find consuming MUL instruction reading two temp operands")
PY

printf 'PASS: step-register-collision-test\n'
