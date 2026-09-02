#!/usr/bin/env bash
# A float4 constructor's VECTOR operands fill one lane per component
# (t_14d18f02).  The vertex lane classifier used each operand's argument
# position as its destination lane, so a float2 or an `.xyz` swizzle
# counted as one: the following literals landed early and the tail lanes
# were never written.  Silent, in the shipping path.
#
# The assertions are the NV40 vertex write masks the instructions carry,
# because "which lanes get written" is the whole defect.  A position with
# two lanes unwritten does not rasterise; a texcoord with w unwritten
# reads whatever the interpolator had.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-vp-construct-lanes-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {   # $1 shader, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx --legacy-lowering "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 failed to compile"
    fi
}

narrow="$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_construct_lanes_v.cg"
swizzle="$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_construct_swizzle_v.cg"
[[ -f "$narrow" ]]  || fail "fixture missing: $narrow"
[[ -f "$swizzle" ]] || fail "fixture missing: $swizzle"

compile "$narrow" narrow
compile "$swizzle" swizzle

python3 - "$work/narrow.log" "$work/swizzle.log" <<'PY'
import re
import sys


def masks(path):
    """NV40 VP vector write mask of each instruction, X in the high bit.
    hw[3] bits 13..16 - the encoding the assembler writes."""
    out = []
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if not m:
            continue
        words = [int(w, 16) for w in m.group(2).split()]
        if len(words) < 4:
            raise SystemExit("FAIL: a vertex instruction must be four words")
        out.append((words[3] >> 13) & 0xF)
    return out


def show(ms):
    return ", ".join("0x%x" % m for m in ms)


# --- float2 operand: out_texcoord = float4(in_texcoord, 0, 1) ---------
# MOV o[0] (xyzw) ; MOV o[7].xy from the input ; MOV o[7].zw from consts.
narrow = masks(sys.argv[1])
if narrow != [0xF, 0xC, 0x3]:
    raise SystemExit(
        "FAIL: vp_construct_lanes_v must write o[0] whole (0xf), then the "
        "float2 input into xy (0xc) and the two literals into zw (0x3); got "
        "%s.  The pre-fix shape was 0xf, 0x8, 0x6 - one lane for the whole "
        "float2, the literals shifted down, and w never written "
        "(t_14d18f02)." % show(narrow)
    )

# --- .xyz swizzle operand: out_position = float4(in_position.xyz, 1) --
# Four writes: position xyz and w, texcoord xy and zw.  Compared as a
# SET: the reference interleaves the trailing literal write differently
# from us, which is an ordering difference and not this defect.
swizzle = sorted(masks(sys.argv[2]))
if swizzle != [0x1, 0x3, 0xC, 0xE]:
    raise SystemExit(
        "FAIL: vp_construct_swizzle_v must cover both outputs completely - "
        "position 0xe + 0x1 and texcoord 0xc + 0x3, in some order; got %s.  "
        "The pre-fix shape was 0x4, 0x6, 0x8, 0x8, which leaves position "
        "with two lanes never written, so the quad does not rasterise "
        "(t_14d18f02)." % show(swizzle)
    )
PY

printf 'vp-construct-lanes-test: ok\n'
