#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_write_mask_clobber.fcg"
work="${TMPDIR:-/tmp}/ps3dk-shader-mask-clobber-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

log="$work/fp_write_mask_clobber.log"
rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$log" 2>&1 || rc=$?

if [[ "$rc" -eq 124 ]]; then
    fail "fp_write_mask_clobber timed out"
fi
if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
    fail "fp_write_mask_clobber aborted or was killed under memory cap"
fi
if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$log"; then
    fail "fp_write_mask_clobber reported an allocation abort"
fi
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$log" >&2
    fail "fp_write_mask_clobber failed to compile"
fi

python3 - "$log" <<'PY'
import re
import sys

log_path = sys.argv[1]
mul_masks = []
for line in open(log_path, "r", encoding="utf-8"):
    m = re.match(r"\s*\d+:\s+([0-9a-fA-F]{8})\s+", line)
    if not m:
        continue
    disk_word = int(m.group(1), 16)
    logical = ((disk_word >> 16) | ((disk_word & 0xffff) << 16)) & 0xffffffff
    opcode = (logical >> 24) & 0x3f
    outmask = (logical >> 9) & 0xf
    if opcode == 0x02:  # NVFX_FP_OP_OPCODE_MUL
        mul_masks.append(outmask)

if len(mul_masks) < 2:
    raise SystemExit(f"FAIL: expected at least two MUL instructions, saw {mul_masks}")

expected = [0x1, 0x2]
if mul_masks[:2] != expected:
    got = ", ".join(f"0x{x:x}" for x in mul_masks[:2])
    raise SystemExit(
        "FAIL: first two MUL writemasks must be x then y; got " + got
    )
PY

printf 'write-mask-clobber-test: ok\n'
