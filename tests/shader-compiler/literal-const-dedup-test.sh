#!/usr/bin/env bash
# A literal vec4 store packs the DISTINCT values and selects with the
# source swizzle, the way the reference compiler does (t_642eb36e).
#
# Both halves are asserted on purpose.  The packed const block read with
# an identity swizzle paints the wrong colour - float4(1,0.5,1,0.5) would
# come out (1,0.5,0,0) - so a test that checked the block alone would pass
# on a miscompile.  The words below are the reference compiler's own, and
# our container is byte-identical to it for both fixtures.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-literal-const-dedup-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {   # $1 shader, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --legacy-lowering "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 failed to compile"
    fi
}

broadcast="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_literal_broadcast_f.cg"
repeat="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_literal_repeat_f.cg"
[[ -f "$broadcast" ]] || fail "fixture missing: $broadcast"
[[ -f "$repeat" ]]    || fail "fixture missing: $repeat"

compile "$broadcast" broadcast
compile "$repeat" repeat

python3 - "$work/broadcast.log" "$work/repeat.log" <<'PY'
import re
import sys


def rows(path):
    out = []
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if m:
            out.append([int(w, 16) for w in m.group(2).split()])
    return out


# Words as the container carries them, read off the compiler's dump and
# confirmed byte-identical to the reference's container for both shaders.
# SRC0 is the second word of the MOV; the const block is the row after it.
CASES = {
    "fp_literal_broadcast_f": {
        "src0": 0x00021C9C,          # c[0].xxxx  - one value, broadcast
        "block": [0x00003F80, 0, 0, 0],
        "why": "float4(1,1,1,1) packs ONE value and broadcasts it",
    },
    "fp_literal_repeat_f": {
        "src0": 0x88021C9C,          # c[0].xyxy  - two values, selected
        "block": [0x00003F80, 0x00003F00, 0, 0],
        "why": "float4(1,0.5,1,0.5) packs TWO values and selects xyxy",
    },
}

for path, name in zip(sys.argv[1:3], ("fp_literal_broadcast_f",
                                      "fp_literal_repeat_f")):
    want = CASES[name]
    r = rows(path)
    if len(r) != 3:
        raise SystemExit(
            "FAIL: %s must emit FENCBR, the MOV and its const block - three "
            "ucode rows, not %d" % (name, len(r))
        )
    if r[1][1] != want["src0"]:
        raise SystemExit(
            "FAIL: %s - %s, so SRC0 must be 0x%08x; got 0x%08x.  An identity "
            "swizzle over a packed block paints the wrong colour."
            % (name, want["why"], want["src0"], r[1][1])
        )
    if r[2] != want["block"]:
        raise SystemExit(
            "FAIL: %s - %s, so the const block must be [%s]; got [%s].  "
            "Writing every lane verbatim is the pre-2026-09-02 shape "
            "(t_642eb36e)."
            % (name, want["why"],
               ", ".join("0x%08x" % w for w in want["block"]),
               ", ".join("0x%08x" % w for w in r[2]))
        )
PY

printf 'literal-const-dedup-test: ok\n'
