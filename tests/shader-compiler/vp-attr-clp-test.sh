#!/usr/bin/env bash
# Explicit VP ATTRn inputs and CLP0 outputs are real application shapes
# (t_63b29467).  The general path used to carry ATTR as a parsed semantic
# but not resolve it to a hardware input, and rejected CLP0 as an output.
#
# The guard checks both layers that can drift independently: the ucode
# must read/write the right hardware slots, and the CgBinaryProgram
# parameter/subtype metadata must advertise the same ATTR/CLP resources.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
attr_src="$shaders/vp_attr_inputs_v.cg"
clp_src="$shaders/vp_clp0_output_v.cg"
[[ -f "$attr_src" ]] || fail "fixture missing: $attr_src"
[[ -f "$clp_src" ]] || fail "fixture missing: $clp_src"

work="${TMPDIR:-/tmp}/ps3dk-vp-attr-clp-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {   # $1 source, $2 output stem
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx --emit-container "$work/$2.vpo" "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 failed to compile on the shipping general path"
    fi
}

compile "$attr_src" attr
compile "$clp_src" clp

python3 - "$work/attr.vpo" "$work/clp.vpo" <<'PY'
import struct
import sys


def load(path):
    blob = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
    cstr = lambda o: "" if not o else blob[o:blob.index(b"\0", o)].decode("ascii", "replace")
    return blob, u32, cstr


def params(blob, u32, cstr):
    out = []
    pcount, parr = u32(12), u32(16)
    for i in range(pcount):
        base = parr + i * 48
        out.append({
            "type": u32(base + 0),
            "res": u32(base + 4),
            "var": u32(base + 8),
            "res_index": u32(base + 12),
            "name": cstr(u32(base + 16)),
            "semantic": cstr(u32(base + 28)),
            "direction": u32(base + 32),
        })
    return out


def vp_subtype(u32):
    prog = u32(20)
    return {
        "instruction_count": u32(prog + 0),
        "register_count": u32(prog + 8),
        "attribute_input_mask": u32(prog + 12),
        "attribute_output_mask": u32(prog + 16),
        "user_clip_mask": u32(prog + 20),
    }


def ucode_words(u32):
    size, off = u32(24), u32(28)
    return [u32(off + i * 4) for i in range(size // 4)]


CG_IN = 0x1001
CG_OUT = 0x1002
CG_VARYING = 0x1005
CG_ATTR0 = 2113
CG_HPOS = 2243
CG_CLP0 = 2310
ATTR_MASK = (1 << 0) | (1 << 3) | (1 << 8) | (1 << 9)

attr_blob, attr_u32, attr_cstr = load(sys.argv[1])
attr_params = params(attr_blob, attr_u32, attr_cstr)
attr_by_sem = {
    p["semantic"]: p for p in attr_params
    if p["var"] == CG_VARYING and p["direction"] == CG_IN
}
for sem, want in {
    "ATTR0": CG_ATTR0 + 0,
    "ATTR3": CG_ATTR0 + 3,
    "ATTR8": CG_ATTR0 + 8,
    "ATTR9": CG_ATTR0 + 9,
}.items():
    got = attr_by_sem.get(sem)
    if got is None:
        raise SystemExit("FAIL: missing VP input parameter with semantic %s" % sem)
    if got["res"] != want:
        raise SystemExit(
            "FAIL: %s resource is %u; expected CG_ATTR%d (%u)"
            % (sem, got["res"], want - CG_ATTR0, want)
        )

attr_meta = vp_subtype(attr_u32)
if attr_meta["attribute_input_mask"] != ATTR_MASK:
    raise SystemExit(
        "FAIL: ATTR fixture attributeInputMask is 0x%08x; expected 0x%08x"
        % (attr_meta["attribute_input_mask"], ATTR_MASK)
    )

clp_blob, clp_u32, clp_cstr = load(sys.argv[2])
clp_params = params(clp_blob, clp_u32, clp_cstr)
clp_by_sem = {
    p["semantic"]: p for p in clp_params
    if p["var"] == CG_VARYING and p["direction"] == CG_OUT
}
clp = clp_by_sem.get("CLP0")
if clp is None:
    raise SystemExit("FAIL: missing VP output parameter with semantic CLP0")
if clp["res"] != CG_CLP0:
    raise SystemExit(
        "FAIL: CLP0 resource is %u; expected CG_CLP0 (%u)" %
        (clp["res"], CG_CLP0)
    )

clp_meta = vp_subtype(clp_u32)
if clp_meta["attribute_output_mask"] != 0x40:
    raise SystemExit(
        "FAIL: CLP fixture attributeOutputMask is 0x%08x; expected 0x00000040"
        % clp_meta["attribute_output_mask"]
    )
if clp_meta["user_clip_mask"] != 0x2:
    raise SystemExit(
        "FAIL: CLP fixture userClipMask is 0x%08x; expected 0x00000002"
        % clp_meta["user_clip_mask"]
    )

# VP ucode is big-endian raw words.  Word 3 bits 2..6 carry the output
# destination for instructions writing the result file.  Measured against
# sce-cgc, CLP0 advertises CG_CLP0 but encodes through destination 5; the
# clip-specific metadata above distinguishes it from a FOGC output.
dests = [(w >> 2) & 0x1F for w in ucode_words(clp_u32)[3::4]]
if 5 not in dests:
    raise SystemExit(
        "FAIL: CLP fixture ucode never writes the reference CLP0 destination (5); destinations were %s"
        % ", ".join(str(d) for d in dests)
    )
PY

printf 'vp-attr-clp-test: ok\n'
