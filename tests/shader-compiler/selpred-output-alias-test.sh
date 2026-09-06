#!/usr/bin/env bash
# t_652d6e42: output-pinned SelPred must not allocate its destination in
# the same R slot as the condition or then-value source it reads later.
#
# SelPred expands as:
#   MOV dst, else
#   MOVC CC.x, cond
#   MOV dst(NE.x), then
#
# That is not read-then-write internally.  If dst aliases src0 or src1,
# the first MOV clobbers a value the later expanded instructions still
# need, producing an always-else select.  The fixture is a tracked copy of
# the test_76 shape that exposed this after control-flow lowering started
# composing COLOR0 lane-by-lane into output-pinned R0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-selpred-output-alias-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_selpred_output_alias_f.cg"
out="$work/fp_selpred_output_alias.fpo"
log="$work/fp_selpred_output_alias.log"
[[ -f "$src" ]] || fail "fixture missing: $src"

(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" env RSX_DUMP_ORDER=1 \
        "$compiler" -p sce_fp_rsx --emit-container "$out" "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "fp_selpred_output_alias_f.cg did not compile"
}

[[ -s "$out" ]] || fail "compiler exited 0 but produced no container"

python3 - "$log" <<'PY'
import re
import sys

log_path = sys.argv[1]
alloc_re = re.compile(
    r"alloc\[(\d+)\] op=\d+ opName=SelPred dstOut=(\d+) dstIdx=(\d+) "
    r"dstPhys=(-?\d+) dstFp16=(\d+)"
)
src_re = re.compile(
    r"\s+src([012]) kind=(\d+) idx=(\d+) phys=(-?\d+) fp16=(\d+)"
)

selpreds = []
current = None
with open(log_path, encoding="utf-8", errors="replace") as handle:
    for line in handle:
        m = alloc_re.match(line)
        if m:
            current = {
                "instr": int(m.group(1)),
                "dst_phys": int(m.group(4)),
                "dst_fp16": int(m.group(5)) != 0,
                "srcs": {},
            }
            selpreds.append(current)
            continue
        m = src_re.match(line)
        if current is not None and m:
            current["srcs"][int(m.group(1))] = {
                "kind": int(m.group(2)),
                "idx": int(m.group(3)),
                "phys": int(m.group(4)),
                "fp16": int(m.group(5)) != 0,
            }

if not selpreds:
    raise SystemExit("FAIL: fixture compiled without any SelPred allocation trace")

def slot(phys, fp16):
    return phys >> 1 if fp16 else phys

checked = 0
for row in selpreds:
    dst_slot = slot(row["dst_phys"], row["dst_fp16"])
    for src_index in (0, 1):
        src = row["srcs"].get(src_index)
        if not src or src["kind"] != 1:
            continue
        if src["phys"] < 0:
            raise SystemExit(
                f"FAIL: SelPred at alloc[{row['instr']}] has unresolved temp src{src_index}"
            )
        checked += 1
        src_slot = slot(src["phys"], src["fp16"])
        if src_slot == dst_slot:
            raise SystemExit(
                f"FAIL: SelPred at alloc[{row['instr']}] writes R{dst_slot} "
                f"but early-read src{src_index} v{src['idx']} also occupies R{src_slot}; "
                "the expanded default MOV would clobber a later source "
                "(t_652d6e42 regression)"
            )

if checked == 0:
    raise SystemExit("FAIL: no temp condition/then sources were checked")

print(f"selpred-output-alias-test: ok ({len(selpreds)} SelPred nodes, {checked} early temp sources)")
PY

printf 'PASS: selpred-output-alias-test\n'
