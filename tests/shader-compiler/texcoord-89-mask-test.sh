#!/usr/bin/env bash
# TEXCOORD8 and TEXCOORD9 use the high fragment-input selector values
# TC(8) / TC(9), but the container metadata does not continue the TC0..7
# attributeInputMask sequence at bits 22/23.  The reference compiler sets
# bits 12/13 instead, and still participates in texCoordsInputMask /
# texCoords2D as TEXCOORD8/9.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-texcoord-89-mask-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

for n in 8 9; do
    src="$work/tc$n.fcg"
    cat >"$src" <<CG
void main(float4 v : TEXCOORD$n, out float4 color : COLOR) {
    color = v * float4(0.5f, 0.5f, 0.5f, 0.5f) +
            float4(0.5f, 0.5f, 0.5f, 0.5f);
}
CG

# Shelf-life: when the retired legacy matcher is removed, drop this second
# --legacy-lowering run and its header claim in the same commit.
    for path in general legacy; do
        flags=()
        [[ "$path" == legacy ]] && flags=(--legacy-lowering)
        out="$work/tc${n}_$path.fpo"
        rc=0
        (
            ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
            timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
                -p sce_fp_rsx "${flags[@]}" --emit-container "$out" "$src"
        ) >"$work/tc${n}_$path.log" 2>&1 || rc=$?
        [[ "$rc" -eq 124 ]] && fail "TEXCOORD$n ($path) timed out"
        if [[ "$rc" -ne 0 ]]; then
            tail -n 20 "$work/tc${n}_$path.log" >&2
            fail "TEXCOORD$n failed to compile on the $path path"
        fi

        python3 - "$out" "$path" "$n" <<'PY'
import struct
import sys

blob, path, n = open(sys.argv[1], "rb").read(), sys.argv[2], int(sys.argv[3])
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
u16 = lambda o: struct.unpack_from(">H", blob, o)[0]

prog = u32(20)
attr = u32(prog + 4)
tc_in = u16(prog + 12)
tc_2d = u16(prog + 14)

# Measured against the reference: TC8/9 occupy attribute bits 12/13.
want_attr = 1 << (n + 4)
want_tc = 1 << n

if attr != want_attr:
    raise SystemExit(
        "FAIL(%s TEXCOORD%d): attributeInputMask is 0x%08x; expected 0x%08x"
        % (path, n, attr, want_attr)
    )
if (tc_in & want_tc) == 0:
    raise SystemExit(
        "FAIL(%s TEXCOORD%d): texCoordsInputMask is 0x%04x; expected bit %d"
        % (path, n, tc_in, n)
    )
if (tc_2d & want_tc) != 0:
    raise SystemExit(
        "FAIL(%s TEXCOORD%d): texCoords2D is 0x%04x; float4 read must clear bit %d"
        % (path, n, tc_2d, n)
    )
PY
    done
done

printf 'texcoord-89-mask-test: ok\n'
