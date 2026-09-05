#!/usr/bin/env bash
# t_0a4e0ed4: The FP scheduler must model RAW dependency edges for partial-writemask
# writers to the same register, preventing consumers from being scheduled before
# earlier lane writes.
#
# Measured on test_02_add.fcg: `float3 result = col1 + col2; output.color = float4(result, 1.0);`
# Under the defect, the final ADD was scheduled at instruction 10 while operand
# lane writes were emitted at instructions 14 and 23.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-scheduler-raw-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
src="$shaders/test_02_add.fcg"
[[ -f "$src" ]] || fail "fixture missing: $src"

log="$work/test_02_add.log"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "test_02_add.fcg did not compile"
}

python3 - "$repo_root/tests/shader-compiler" "$log" <<'PY'
import sys, os
sys.path.insert(0, sys.argv[1])
from ucode_decode import groups

gs = groups(sys.argv[2])
if not gs:
    sys.exit("FAIL: no ucode groups decoded from log")

# Track all writers for each temp register: reg -> list of instruction indices
writers = {}
for i, w in enumerate(gs):
    none = (w[0] >> 30) & 1
    dst = (w[0] >> 1) & 0x3f
    if not none:
        writers.setdefault(dst, []).append(i)

# Find the vector ADD instruction that computes col1 + col2 into R0.xyz
# Opcode 0x03 (ADD), dst=R0, mask has .xyz bits set (0x7)
add_idx = None
add_srcs = []
for i, w in enumerate(gs):
    opcode = (w[0] >> 24) & 0x3f
    dst = (w[0] >> 1) & 0x3f
    mask = (w[0] >> 9) & 0xf
    if opcode == 0x03 and dst == 0 and (mask & 0x7) != 0:
        add_idx = i
        src0_type = w[1] & 3
        src0_reg = (w[1] >> 2) & 0x3f
        src1_type = w[2] & 3
        src1_reg = (w[2] >> 2) & 0x3f
        if src0_type == 0:  # TEMP
            add_srcs.append(src0_reg)
        if src1_type == 0:  # TEMP
            add_srcs.append(src1_reg)
        break

if add_idx is None:
    sys.exit("FAIL: could not find final ADD instruction writing to R0.xyz")

if not add_srcs:
    sys.exit("FAIL: final ADD did not read temporary register operands")

# Assert that add_idx is strictly greater than every instruction that writes
# to either of its source operand registers
for src_reg in add_srcs:
    reg_writers = writers.get(src_reg, [])
    for w_idx in reg_writers:
        if w_idx >= add_idx:
            sys.exit(
                f"FAIL: RAW hazard on R{src_reg}: writer at instruction {w_idx} "
                f"was scheduled after or at the consuming ADD at instruction {add_idx} "
                f"(t_0a4e0ed4 regression)"
            )

print(f"scheduler-partial-writer-raw-test: ok (ADD at {add_idx} correctly follows all operand writers {add_srcs})")
PY

printf 'PASS: scheduler-partial-writer-raw-test\n'
