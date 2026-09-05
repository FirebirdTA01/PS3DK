#!/usr/bin/env bash
# texCoords2D must follow the DECLARED width of each TEXCOORD varying, not
# the lanes the shader consumes (t_f5f750ff follow-on).  Marking a float3 or
# float4 varying as 2D is how a varying's z or w stops being what the vertex
# program wrote - which no amount of compiling successfully will reveal, so
# this reads the container's FP sub-header directly.
#
# BOTH lowering paths.  An earlier version ran only the default path and so
# could not see that the general path still decided the field at the use
# site with the old consumed-lanes rule (review finding, codex).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_texcoord_2d_flags_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-texcoord-2d-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# Shelf-life: when the retired legacy matcher is removed, drop this second
# --legacy-lowering run and its header claim in the same commit.
for path in general legacy; do
    flags=()
    [[ "$path" == legacy ]] && flags=(--legacy-lowering)
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "${flags[@]}" \
            --emit-container "$work/$path.fpo" "$src"
    ) >"$work/$path.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "fp_texcoord_2d_flags ($path) timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$path.log" >&2
        fail "fp_texcoord_2d_flags failed to compile on the $path path"
    fi

    python3 - "$work/$path.fpo" "$path" <<'PY'
import struct, sys

blob, path = open(sys.argv[1], "rb").read(), sys.argv[2]
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
u16 = lambda o: struct.unpack_from(">H", blob, o)[0]

prog  = u32(20)
tc_in = u16(prog + 12)      # texCoordsInputMask
tc_2d = u16(prog + 14)      # texCoords2D

# Reference value for this fixture, confirmed against the reference
# compiler directly: 0xFFE6.
cases = [
    (0, False, "declared float4 and READ: must be cleared"),
    (1, True,  "declared float2 and read: must stay SET - clearing it is "
               "over-broad"),
    (2, True,  "declared float4 but NEVER READ: must stay SET - an unread "
               "varying is not interpolated"),
    (3, False, "declared float4 and READ through the TEX spelling: must be "
               "cleared. TEX3 is TEXCOORD3; a width map that string-matches "
               "'TEXCOORD' misses it while the read mask still gets set"),
    (4, False, "declared float4 and read ONLY as a tex2D coordinate: must be "
               "cleared. This is the sd_tex_ctrl shape and the case that "
               "separates the declared-width rule from a consumed-lanes one - "
               "tex2D touches only .xy, yet the reference clears it"),
]

# Guard the guard: if the read mask is not what the shader implies, the
# assertions below are about a different program than the one described.
if tc_in & 0x1F != 0x1B:
    raise SystemExit(
        "FAIL(%s): texCoordsInputMask is 0x%04x; expected TEXCOORD0, "
        "TEXCOORD1, TEX3 and TEXCOORD4 read with TEXCOORD2 unread (0x1B in "
        "the low five bits). The fixture is not exercising the cases it "
        "names." % (path, tc_in)
    )

for n, want_set, why in cases:
    got_set = bool((tc_2d >> n) & 1)
    if got_set != want_set:
        raise SystemExit(
            "FAIL(%s): texCoords2D bit %d is %s (field 0x%04x); %s"
            % (path, n, "set" if got_set else "cleared", tc_2d, why)
        )
PY
done

printf 'texcoord-2d-flags-test: ok\n'
