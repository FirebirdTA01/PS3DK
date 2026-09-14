#!/usr/bin/env bash
# A default value on an ENTRY PARAMETER: where it is legal, where it is not,
# and what it records (t_4b54f26b A1).
#
# The reference's rule, measured, and it has a named diagnostic for it:
#   error C1114: only uniform parameters to the entry function can have
#   default values: "<name>"
# So legal on a UNIFORM parameter of the SELECTED entry, legal on any
# parameter of a helper, illegal everywhere else.
#
# THREE THINGS THIS ASSERTS, and the third is the one a name heuristic fails:
#  1. the four accepted spellings record the reference's exact values, in the
#     parameter table AND in the ucode's inline constant - independent places,
#     and getting only the first right renders wrong while reflection reads
#     correctly;
#  2. the two refusals exit 1 with the right diagnostic CLASS, one permanent
#     (non-uniform entry parameter) and one INTERIM (helper default, deleted
#     by t_36492ad8);
#  3. the same function is illegal under -e alpha and hits a DIFFERENT refusal
#     under -e beta, from one unchanged source - which is what proves the
#     check keys on the SELECTED entry rather than on the name "main".
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -z "$compiler" ]] && compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="${TMPDIR:-/tmp}/ps3dk-entry-param-default-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run() {  # profile src tag [entry]
    local profile="$1" src="$2" tag="$3" entry="${4:-}" rc=0 args=()
    [[ -f "$src" ]] || fail "fixture missing: $src"
    [[ -n "$entry" ]] && args+=(-e "$entry")
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" "${args[@]}" --emit-container "$work/$tag.bin" "$src"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    echo "$rc"
}

# ---- 1. ACCEPTED: the four spellings, values in both places. ----
rc=$(run sce_fp_rsx "$shaders/fp_entry_param_default_f.cg" accept)
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/accept.log" >&2
    fail "fp_entry_param_default_f should compile (exit $rc)"
fi

python3 - "$work/accept.bin" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
unswap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF
def cstr(o):
    return "" if not o else blob[o:blob.index(bytes([0]), o)].decode("ascii", "replace")
n, parr, ucoff = u32(12), u32(16), u32(28)
rec = {}
for i in range(n):
    o = parr + i * 48
    doff, ec = u32(o + 20), u32(o + 24)
    vals = ([round(struct.unpack_from(">f", blob, doff + 4 * k)[0], 6) for k in range(4)]
            if doff else None)
    inline = None
    if ec and u32(ec):
        off = u32(ec + 4)
        w = [unswap(struct.unpack_from(">I", blob, ucoff + off + 4 * j)[0]) for j in range(4)]
        inline = [round(struct.unpack(">f", struct.pack(">I", x))[0], 6) for x in w]
    rec[cstr(u32(o + 16))] = {"def": doff, "vals": vals, "inline": inline}

# Measured on the reference for these exact spellings.
want = {"light": [1.0, 2.0, 3.0, 4.0],     # braced
        "Ka":    [0.6, 0.6, 0.6, 0.0],     # SCALAR broadcast into a float3
        "l":     [1.0, 2.0, 3.0, 0.0],     # constructor
        "s":     [2.5, 0.0, 0.0, 0.0]}     # scalar into a scalar
errs = []
for name, v in want.items():
    r = rec.get(name)
    if r is None:
        errs.append(f"parameter {name!r} is not in the container"); continue
    if not r["def"]:
        errs.append(f"{name}: no defaultValue block; expected {v}")
    elif r["def"] % 16:
        errs.append(f"{name}: block at {r['def']} is not 16-byte aligned")
    elif r["vals"] != v:
        errs.append(f"{name}: defaultValue is {r['vals']}, expected {v}")
    # The half a reflection-only check cannot see.
    if r["inline"] != v:
        errs.append(f"{name}: INLINE CONSTANT is {r['inline']}, expected {v} - "
                    "an unpatched shader computes with this, not with the table")
# CONTROL: the varying carries no default.
t = rec.get("texcoord")
if t is not None and t["def"]:
    errs.append(f"control texcoord has a defaultValue at {t['def']} ({t['vals']})")
if errs:
    for e in errs: print("FAIL: " + e, file=sys.stderr)
    sys.exit(1)
print("  accepted spellings: " + " ".join(f"{k}={v}" for k, v in want.items()))
PY

# ---- 2. REFUSALS, by diagnostic class, not merely by status. ----
expect_refusal() {  # tag needle description
    local tag="$1" needle="$2" what="$3"
    local rc; rc=$(cat "$work/$tag.rc")
    [[ "$rc" -eq 1 ]] || fail "$what: expected exit 1, got $rc"
    grep -q "$needle" "$work/$tag.log" || {
        tail -n 5 "$work/$tag.log" >&2
        fail "$what: exit 1 but not for the expected reason ($needle)"
    }
}
run sce_fp_rsx "$shaders/fp_entry_param_default_refuse_f.cg" nonuniform > "$work/nonuniform.rc"
expect_refusal nonuniform "only uniform parameters to the entry function" \
    "a default on a non-uniform ENTRY parameter (permanent, C1114)"

run sce_fp_rsx "$shaders/fp_helper_param_default_refuse_f.cg" helper > "$work/helper.rc"
expect_refusal helper "is not the entry function" \
    "a default on a HELPER parameter (interim, t_36492ad8)"

# ---- 3. THE ENTRY-SWAP CONTROL: one source, two entries, two classes. ----
run sce_fp_rsx "$shaders/fp_entry_swap_default_f.cg" swapalpha alpha > "$work/swapalpha.rc"
expect_refusal swapalpha "only uniform parameters to the entry function" \
    "-e alpha: alpha is the ENTRY, so C1114 applies"

run sce_fp_rsx "$shaders/fp_entry_swap_default_f.cg" swapbeta beta > "$work/swapbeta.rc"
expect_refusal swapbeta "is not the entry function" \
    "-e beta: alpha is a HELPER, so the interim refusal applies instead"
# If those two ever produce the SAME diagnostic, the check has stopped keying
# on the selected entry - that is the whole point of this pair.
if cmp -s <(grep -o "error: [a-z ]*" "$work/swapalpha.log" | head -1) \
          <(grep -o "error: [a-z ]*" "$work/swapbeta.log" | head -1); then
    fail "entry swap produced the same diagnostic class for both entries; "\
"the check is not keying on the SELECTED entry"
fi

# ---- 4. THE VERTEX PROFILE, and a MATRIX entry parameter on both surfaces.
# A1 first wrote the entry block into the FRAGMENT container only, with only
# fragment fixtures, so the vertex side dropped every default and nothing
# could see it; and the matrix seeding sat in a branch a matrix entry
# parameter never reaches, so its table was right while its ucode was zero.
# Both were found by reviewers probing what the fixtures did not cover.
rc=$(run sce_vp_rsx "$shaders/vp_entry_param_default_v.vcg" vpdef)
[[ "$rc" -eq 0 ]] || { tail -n 20 "$work/vpdef.log" >&2; fail "vp_entry_param_default should compile (exit $rc)"; }
rc=$(run sce_fp_rsx "$shaders/fp_entry_matrix_default_f.cg" fpmat)
[[ "$rc" -eq 0 ]] || { tail -n 20 "$work/fpmat.log" >&2; fail "fp_entry_matrix_default should compile (exit $rc)"; }

python3 - "$work/vpdef.bin" "$work/fpmat.bin" <<'PY'
import struct, sys
def load(path):
    blob = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", blob, o)[0]
    unswap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF
    def cstr(o):
        return "" if not o else blob[o:blob.index(bytes([0]), o)].decode("ascii", "replace")
    n, parr, ucoff = u32(12), u32(16), u32(28)
    out = {}
    for i in range(n):
        o = parr + i * 48
        doff, ec = u32(o + 20), u32(o + 24)
        vals = ([round(struct.unpack_from(">f", blob, doff + 4 * k)[0], 6) for k in range(4)]
                if doff else None)
        inline = None
        if ec and u32(ec):
            off = u32(ec + 4)
            w = [unswap(struct.unpack_from(">I", blob, ucoff + off + 4 * j)[0]) for j in range(4)]
            inline = [round(struct.unpack(">f", struct.pack(">I", x))[0], 6) for x in w]
        out[cstr(u32(o + 16))] = {"def": doff, "vals": vals, "inline": inline}
    return out

errs = []
vp = load(sys.argv[1])
for name, v in {"tint": [0.25, 0.5, 0.75, 1.0],
                "M[0]": [1.0, 2.0, 3.0, 0.0],
                "M[1]": [4.0, 5.0, 6.0, 0.0],
                "M[2]": [7.0, 8.0, 9.0, 0.0]}.items():
    r = vp.get(name)
    if r is None:
        errs.append(f"vp: record {name!r} is missing"); continue
    if not r["def"]:
        errs.append(f"vp: {name} has no defaultValue block; expected {v} - the "
                    "VERTEX container is dropping entry-parameter defaults")
    elif r["vals"] != v:
        errs.append(f"vp: {name} defaultValue is {r['vals']}, expected {v}")
for name in ("M", "plain"):
    r = vp.get(name)
    if r is not None and r["def"]:
        errs.append(f"vp: control {name} must have no defaultValue, found {r['vals']}")

fp = load(sys.argv[2])
for name, v in {"M[0]": [1.0, 2.0, 3.0, 0.0],
                "M[1]": [4.0, 5.0, 6.0, 0.0],
                "M[2]": [7.0, 8.0, 9.0, 0.0]}.items():
    r = fp.get(name)
    if r is None:
        errs.append(f"fp-matrix-entry: record {name!r} is missing"); continue
    if r["vals"] != v:
        errs.append(f"fp-matrix-entry: {name} defaultValue is {r['vals']}, expected {v}")
    if r["inline"] != v:
        errs.append(f"fp-matrix-entry: {name} INLINE CONSTANT is {r['inline']}, "
                    f"expected {v} - an unpatched shader multiplies by this")
if fp.get("M", {}).get("def"):
    errs.append("fp-matrix-entry: the matrix PARENT must carry no block")

if errs:
    for e in errs: print("FAIL: " + e, file=sys.stderr)
    sys.exit(1)
print("  vertex entry defaults and matrix entry rows: table and ucode both correct")
PY

# ---- 5. THE SHAPE TABLE.  Ten one-line spellings, measured on the reference,
# kept together IN THIS FILE rather than as ten repo fixtures: the contract is
# only legible as a table, and a reader has to see which forms broadcast
# beside which forms refuse.  A1's first version got four of these wrong
# because it looked at the EVALUATED COMPONENT COUNT alone, which makes `{2}`,
# `float2(1)` and `0.6f` indistinguishable.
shape_case() {  # decl expr expect tag
    local decl="$1" expr="$2" expect="$3" tag="$4" rc
    cat > "$work/$tag.cg" <<EOF
void main(float2 uv : TEXCOORD0, uniform $decl, out float4 c : COLOR) { c = $expr; }
EOF
    rc=$(run sce_fp_rsx "$work/$tag.cg" "$tag")
    if [[ "$expect" == accept && "$rc" -ne 0 ]]; then
        tail -n 3 "$work/$tag.log" >&2
        fail "shape '$decl': the reference ACCEPTS this; we exited $rc"
    fi
    if [[ "$expect" == refuse && "$rc" -ne 1 ]]; then
        fail "shape '$decl': the reference REFUSES this; we exited $rc"
    fi
}
shape_case "float4 u = float4(2)"        "u*uv.x"             accept s1
shape_case "float4 u = 2"                "u*uv.x"             accept s2
shape_case "float4 u = {1,2,3,4}"        "u*uv.x"             accept s3
shape_case "float3 u = 0.6f"             "float4(u,1)*uv.x"   accept s4
shape_case "float2 u = float4(1,2,3,4)"  "float4(u,0,1)*uv.x" accept s5
shape_case "float4 u = {2}"              "u*uv.x"             refuse s6
shape_case "float3 u = {1,2}"            "float4(u,1)*uv.x"   refuse s7
shape_case "float4 u = {1,2,3,4,5}"      "u*uv.x"             refuse s8
shape_case "float4 u = float2(1)"        "u*uv.x"             refuse s9
shape_case "float4 u = float2(1,2,3,4)"  "u*uv.x"             refuse s10
# An ARRAY default: the reference ACCEPTS it and records a block per element;
# we refuse by name until t_2b592fc7 measures the element rules.  The row is
# here so the interim refusal cannot quietly become accept-and-drop - the
# failure mode this whole slice kept producing.  When that card lands, this
# expectation flips to accept WITH the per-element blocks checked.
shape_case "float4 a[2] = { float4(1,2,3,4), float4(5,6,7,8) }"            "a[0]*uv.x + a[1]*uv.y"                             refuse s11

echo "PASS: entry-param-default: four spellings in table and ucode; vertex ""entry defaults and matrix entry rows correct on both surfaces; ten measured ""shapes agree with the reference; non-uniform entry default refused ""(permanent); helper default refused (interim, t_36492ad8); entry swap gives ""different classes from one source"
