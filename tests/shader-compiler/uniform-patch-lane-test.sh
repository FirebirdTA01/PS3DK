#!/usr/bin/env bash
# A uniform's inline const blocks must be readable by a runtime patch of
# that uniform (t_f5f750ff).
#
# cellGcmSetFragmentProgramParameter transfers cols words, starting at
# lane x, to every ucode offset listed in the parameter's embedded-constant
# record.  So for a SCALAR uniform every one of those sources must read
# lane x.  Emitting the destination's lane instead compiles and renders
# correctly unpatched, and silently ignores the application's value in
# every channel but x - which is why this is checked in the container
# rather than by compiling alone.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_scalar_uniform_lanes_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-uniform-patch-lane-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$work/out.fpo" "$src"
) >"$work/out.log" 2>&1 || rc=$?
[[ "$rc" -eq 124 ]] && fail "fp_scalar_uniform_lanes timed out"
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/out.log" >&2
    fail "fp_scalar_uniform_lanes failed to compile"
fi

python3 - "$work/out.fpo" <<'PY'
import struct, sys

blob = open(sys.argv[1], "rb").read()
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
unswap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF

def cstr(o):
    return "" if not o else blob[o:blob.index(b"\0", o)].decode("ascii", "replace")

pcount, parr, ucode = u32(12), u32(16), u32(28)

checked = 0
for i in range(pcount):
    base = parr + i * 48
    if cstr(u32(base + 16)) != "k":
        continue
    emb = u32(base + 24)
    if not emb:
        raise SystemExit(
            "FAIL: uniform 'k' has no embedded-constant record, so nothing "
            "at runtime can patch it"
        )
    for n in range(u32(emb)):
        off = u32(emb + 4 + 4 * n)
        insn = off - 16                      # the block follows its instruction
        if insn < 0:
            raise SystemExit("FAIL: const block at 0x%x has no instruction" % off)
        words = [unswap(u32(ucode + insn + 4 * j)) for j in range(4)]
        for pos in (1, 2, 3):
            w = words[pos]
            if (w & 3) != 2:                 # not the CONST source
                continue
            swz = [(w >> (9 + 2 * k)) & 3 for k in range(4)]
            checked += 1
            if any(c != 0 for c in swz):
                raise SystemExit(
                    "FAIL: scalar uniform 'k' is read as .%s at ucode 0x%x; a "
                    "by-name patch writes lane x only, so this site can never "
                    "see the application's value"
                    % ("".join("xyzw"[c] for c in swz), off)
                )

# Distinguish "every site is correct" from "no site was examined".
if checked < 3:
    raise SystemExit(
        "FAIL: expected k to be read at three const sites, examined %d - the "
        "assertion above proves nothing if the sites are not there" % checked
    )
PY

printf 'uniform-patch-lane-test: ok\n'
