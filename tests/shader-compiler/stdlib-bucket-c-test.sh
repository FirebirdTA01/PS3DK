#!/usr/bin/env bash
# Bucket (c) stdlib test: f4tex2D, h2tex2D, h3texCUBE, tex2D(sampler2D, float3),
# tex3D, tex2Dbias (FP), lit intrinsic hardware opcode (0x3C/LITEX2) and zero-exponent
# edge optimization, matrix transpose in VP/FP (constructed and uniform float3x3),
# half-texture precision boundary, and exact rc=1 refusal for VP texture fetch.
#
# Validates nonempty containers, decoded ucode opcodes, parameter tables,
# lit hardware opcodes and zero-exponent NaN guards, transpose element/twin checks,
# and exact rc=1 refusal without container output for VP texture fetch.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

if [[ -z "$compiler" ]]; then
    for cand in "$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler" \
                "$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler.exe" \
                "$repo_root/build-win/rsx-cg-compiler.exe"; do
        if [[ -x "$cand" ]]; then compiler="$cand"; break; fi
    done
fi
if [ ! -x "$compiler" ] && [ -f "${compiler}.exe" ]; then
    compiler="${compiler}.exe"
fi
[[ -n "$compiler" && -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-bucket-c-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# 1. f4tex2D, h2tex2D, h3texCUBE
cat > "$work/typed_tex.cg" << 'EOF'
float4 main(float2 uv : TEXCOORD0,
            uniform sampler2D s2d,
            uniform samplerCUBE scube) : COLOR
{
    float4 a = f4tex2D(s2d, uv);
    half2 b = h2tex2D(s2d, uv);
    half3 c = h3texCUBE(scube, half3(uv.x, uv.y, 1.0));
    return a + float4(b.x, b.y, c.x, c.y);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/typed_tex.fpo" "$work/typed_tex.cg" > /dev/null 2>&1 ||
    fail "typed_tex.cg failed to compile under sce_fp_rsx"
[[ -s "$work/typed_tex.fpo" ]] || fail "typed_tex.fpo is missing or empty"
"$compiler" -p sce_fp_rsx "$work/typed_tex.cg" > "$work/typed_tex.log" 2> "$work/typed_tex.err" ||
    fail "typed_tex.cg listing compile failed"

# 2. tex2D(sampler2D, float3)
cat > "$work/tex2d_f3.cg" << 'EOF'
float4 main(float3 uvw : TEXCOORD0,
            uniform sampler2D s2d) : COLOR
{
    return tex2D(s2d, uvw);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/tex2d_f3.fpo" "$work/tex2d_f3.cg" > /dev/null 2>&1 ||
    fail "tex2d_f3.cg failed to compile under sce_fp_rsx"
[[ -s "$work/tex2d_f3.fpo" ]] || fail "tex2d_f3.fpo is missing or empty"
"$compiler" -p sce_fp_rsx "$work/tex2d_f3.cg" > "$work/tex2d_f3.log" 2> "$work/tex2d_f3.err" ||
    fail "tex2d_f3.cg listing compile failed"

# 3. tex3D and tex2Dbias in FP
cat > "$work/tex3d_bias.cg" << 'EOF'
float4 main(float4 uv_bias : TEXCOORD0,
            uniform sampler2D s2d,
            uniform sampler3D s3d) : COLOR
{
    float4 a = tex3D(s3d, uv_bias.xyz);
    float4 b = tex2Dbias(s2d, uv_bias);
    return a + b;
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/tex3d_bias.fpo" "$work/tex3d_bias.cg" > /dev/null 2>&1 ||
    fail "tex3d_bias.cg failed to compile under sce_fp_rsx"
[[ -s "$work/tex3d_bias.fpo" ]] || fail "tex3d_bias.fpo is missing or empty"
"$compiler" -p sce_fp_rsx "$work/tex3d_bias.cg" > "$work/tex3d_bias.log" 2> "$work/tex3d_bias.err" ||
    fail "tex3d_bias.cg listing compile failed"

# 4. lit intrinsic with hardware LITEX2 (0x3C) and zero-exponent optimization
cat > "$work/lit.cg" << 'EOF'
float4 main(half3 l : TEXCOORD0, half3 h : TEXCOORD1) : COLOR
{
    half4 res = lit(l.x, h.y, 16.0);
    return float4(res);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/lit.fpo" "$work/lit.cg" > /dev/null 2>&1 ||
    fail "lit.cg failed to compile under sce_fp_rsx"
[[ -s "$work/lit.fpo" ]] || fail "lit.fpo is missing or empty"
"$compiler" -p sce_fp_rsx "$work/lit.cg" > "$work/lit.log" 2> "$work/lit.err" ||
    fail "lit.cg listing compile failed"

cat > "$work/lit_zero.cg" << 'EOF'
float4 main(float3 p : TEXCOORD0) : COLOR
{
    return lit(p.x, p.y, 0.0);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/lit_zero.fpo" "$work/lit_zero.cg" > /dev/null 2>&1 ||
    fail "lit_zero.cg failed to compile under sce_fp_rsx"
[[ -s "$work/lit_zero.fpo" ]] || fail "lit_zero.fpo is missing or empty"
"$compiler" -p sce_fp_rsx "$work/lit_zero.cg" > "$work/lit_zero.log" 2> "$work/lit_zero.err" ||
    fail "lit_zero.cg listing compile failed"

cat > "$work/lit_twin.cg" << 'EOF'
float4 main(half3 l : TEXCOORD0, half3 h : TEXCOORD1) : COLOR
{
    float ndotl = l.x;
    float ndoth = h.y;
    float y = max(ndotl, 0.0);
    float h_c = max(ndoth, 0.0);
    float z = (ndotl > 0.0 && ndoth > 0.0) ? pow(h_c, 16.0) : 0.0;
    return float4(1.0, y, z, 1.0);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/lit_twin.fpo" "$work/lit_twin.cg" > /dev/null 2>&1 ||
    fail "lit_twin.cg failed to compile under sce_fp_rsx"
[[ -s "$work/lit_twin.fpo" ]] || fail "lit_twin.fpo is missing or empty"

# 5. transpose in VP + double-transpose twin
cat > "$work/transpose_vp.cg" << 'EOF'
struct VOut {
    float4 pos : POSITION;
    float4 col : COLOR0;
};
VOut main(float4 pos : POSITION,
          uniform float4x4 m)
{
    VOut o;
    float4x4 mt = transpose(m);
    o.pos = mul(mt, pos);
    o.col = pos;
    return o;
}
EOF

"$compiler" -p sce_vp_rsx --emit-container "$work/transpose_vp.vpo" "$work/transpose_vp.cg" > /dev/null 2>&1 ||
    fail "transpose_vp.cg failed to compile under sce_vp_rsx"
[[ -s "$work/transpose_vp.vpo" ]] || fail "transpose_vp.vpo is missing or empty"
"$compiler" -p sce_vp_rsx "$work/transpose_vp.cg" > "$work/transpose_vp.log" 2> "$work/transpose_vp.err" ||
    fail "transpose_vp.cg listing compile failed"

cat > "$work/transpose_vp_double.cg" << 'EOF'
struct VOut {
    float4 pos : POSITION;
};
VOut main(float4 pos : POSITION,
          uniform float4x4 m)
{
    VOut o;
    float4x4 mtt = transpose(transpose(m));
    o.pos = mul(mtt, pos);
    return o;
}
EOF

"$compiler" -p sce_vp_rsx --emit-container "$work/transpose_vp_double.vpo" "$work/transpose_vp_double.cg" > /dev/null 2>&1 ||
    fail "transpose_vp_double.cg failed to compile under sce_vp_rsx"
[[ -s "$work/transpose_vp_double.vpo" ]] || fail "transpose_vp_double.vpo is missing or empty"

# 6. transpose in FP (constructed, element indexing twin, and uniform float3x3)
cat > "$work/transpose_fp.cg" << 'EOF'
float4 main(float3 a : TEXCOORD0, float3 b : TEXCOORD1) : COLOR
{
    float3x3 m = float3x3(a, b, float3(0.0, 1.0, 0.0));
    float3x3 mt = transpose(m);
    return float4(mt[0] + mt[1] + mt[2], 1.0);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/transpose_fp.fpo" "$work/transpose_fp.cg" > /dev/null 2>&1 ||
    fail "transpose_fp.cg failed to compile under sce_fp_rsx"
[[ -s "$work/transpose_fp.fpo" ]] || fail "transpose_fp.fpo is missing or empty"

cat > "$work/transpose_fp_elem.cg" << 'EOF'
float4 main(float3 a : TEXCOORD0, float3 b : TEXCOORD1) : COLOR
{
    float3 c = float3(0.1, 0.2, 0.3);
    float3x3 m = float3x3(a, b, c);
    float3x3 mt = transpose(m);
    float3 diff0 = mt[0] - float3(a.x, b.x, c.x);
    float3 diff1 = mt[1] - float3(a.y, b.y, c.y);
    float3 diff2 = mt[2] - float3(a.z, b.z, c.z);
    return float4(diff0 + diff1 + diff2, 1.0);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/transpose_fp_elem.fpo" "$work/transpose_fp_elem.cg" > /dev/null 2>&1 ||
    fail "transpose_fp_elem.cg failed to compile under sce_fp_rsx"
[[ -s "$work/transpose_fp_elem.fpo" ]] || fail "transpose_fp_elem.fpo is missing or empty"

cat > "$work/transpose_fp_uniform.cg" << 'EOF'
float4 main(float3 p : TEXCOORD0,
            uniform float3x3 m) : COLOR
{
    float3x3 mt = transpose(m);
    return float4(mul(mt, p), 1.0);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/transpose_fp_uniform.fpo" "$work/transpose_fp_uniform.cg" > /dev/null 2>&1 ||
    fail "transpose_fp_uniform.cg failed to compile under sce_fp_rsx"
[[ -s "$work/transpose_fp_uniform.fpo" ]] || fail "transpose_fp_uniform.fpo is missing or empty"

cat > "$work/transpose_fp_double.cg" << 'EOF'
float4 main(float3 p : TEXCOORD0,
            uniform float3x3 m) : COLOR
{
    float3x3 mtt = transpose(transpose(m));
    return float4(mul(mtt, p), 1.0);
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/transpose_fp_double.fpo" "$work/transpose_fp_double.cg" > /dev/null 2>&1 ||
    fail "transpose_fp_double.cg failed to compile under sce_fp_rsx"
[[ -s "$work/transpose_fp_double.fpo" ]] || fail "transpose_fp_double.fpo is missing or empty"

# 6c. transpose moves VALUES between lanes, so the rows above - which only
# establish that a container exists with a non-empty ucode - cannot tell a
# correct transpose from a row read as a column, from one lane off, or from
# the call being dropped entirely.  These three spellings share one matrix
# of literals, so every element is distinguishable from every other, and
# fp_transpose_check.py walks the emitted program and requires the exact
# dot products.  See that file's header for what it refuses to model.
#
# The double spelling is here because the reference folds it: sce-cgc emits
# a BYTE-IDENTICAL container for transpose(transpose(m)) and m, in both
# profiles.  We emit 32 fragment instructions where the direct spelling
# takes 8.  That distance is a scheduling gap, carded separately - and it
# is exactly what a size or shape check would confuse with a broken
# permutation, which is why the assertion below is on the values.
transpose_checker="$repo_root/tests/shader-compiler/fp_transpose_check.py"
[[ -f "$transpose_checker" ]] || fail "fp_transpose_check.py is missing"

emit_literal_matrix_shader() {             # $1 stem, $2 the matrix expression
    cat > "$work/$1.cg" << EOF
float4 main(float3 p : TEXCOORD0) : COLOR
{
    float3x3 m = float3x3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
    return float4(mul($2, p), 1.0);
}
EOF
    "$compiler" -p sce_fp_rsx "$work/$1.cg" > "$work/$1.log" 2> "$work/$1.err" ||
        fail "$1.cg failed to compile under sce_fp_rsx"
}

emit_literal_matrix_shader lanes_direct     "m"
emit_literal_matrix_shader lanes_transposed "transpose(m)"
emit_literal_matrix_shader lanes_double     "transpose(transpose(m))"

lanes() {                                  # $1 stem, $2 expected rows
    "$PYTHON" "$transpose_checker" "$work/$1.log" "$2" ||
        fail "$1: the colour output does not carry $2"
}

lanes lanes_direct     "1,2,3;4,5,6;7,8,9"
lanes lanes_transposed "1,4,7;2,5,8;3,6,9"
lanes lanes_double     "1,2,3;4,5,6;7,8,9"

# CONTROL: the checker must reject the transposed program when handed the
# untransposed expectation, and the other way round - otherwise the three
# rows above are three spellings of "a program was emitted".  Exit 1 alone
# is not enough to prove that: an uncaught exception, a missing file and a
# refusal all exit 1 too, so the rejection must name the lane that did not
# match.
reject_lanes() {                           # $1 stem, $2 a wrong expectation
    local rc=0
    "$PYTHON" "$transpose_checker" "$work/$1.log" "$2" > "$work/$1.control" 2>&1 || rc=$?
    [[ "$rc" -eq 1 ]] ||
        fail "self-check: $1 was not rejected by the expectation $2 (exit $rc)"
    grep -q "^FAIL .*colour output's x lane is" "$work/$1.control" || {
        cat "$work/$1.control" >&2
        fail "self-check: $1 exited 1 without naming a mismatched lane, so the rejection was not the comparison"
    }
}

reject_lanes lanes_direct     "1,4,7;2,5,8;3,6,9"
reject_lanes lanes_transposed "1,2,3;4,5,6;7,8,9"

# 6b. Dead tex2Dbias: eliminated by DCE without alphakill, preserved with #pragma alphakill
cat > "$work/dead_txb.cg" << 'EOF'
float4 main(float4 uv_bias : TEXCOORD0,
            uniform sampler2D s2d,
            uniform sampler2D s_dead) : COLOR
{
    float4 dead = tex2Dbias(s_dead, uv_bias);
    return tex2D(s2d, uv_bias.xy);
}
EOF

cat > "$work/dead_txb_ak.cg" << 'EOF'
#pragma alphakill s_dead
float4 main(float4 uv_bias : TEXCOORD0,
            uniform sampler2D s2d,
            uniform sampler2D s_dead) : COLOR
{
    float4 dead = tex2Dbias(s_dead, uv_bias);
    return tex2D(s2d, uv_bias.xy);
}
EOF

for stem in dead_txb dead_txb_ak; do
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$work/$stem.cg" > /dev/null 2>&1 ||
        fail "$stem.cg failed to compile under sce_fp_rsx"
    [[ -s "$work/$stem.fpo" ]] || fail "$stem.fpo is missing or empty"
    "$compiler" -p sce_fp_rsx "$work/$stem.cg" > "$work/$stem.log" 2> "$work/$stem.err" ||
        fail "$stem.cg listing compile failed"
done

# 7. Half-texture precision boundary (h1tex2D in H bank, precision cast MOV in prec=1)
cat > "$work/h1tex.cg" << 'EOF'
float4 main(float4 p : TEXCOORD0,
            uniform sampler2D s) : COLOR
{
    return float4(h1tex2D(s, p.xy));
}
EOF

"$compiler" -p sce_fp_rsx --emit-container "$work/h1tex.fpo" "$work/h1tex.cg" > /dev/null 2>&1 ||
    fail "h1tex.cg failed to compile under sce_fp_rsx"
[[ -s "$work/h1tex.fpo" ]] || fail "h1tex.fpo is missing or empty"
"$compiler" -p sce_fp_rsx "$work/h1tex.cg" > "$work/h1tex.log" 2> "$work/h1tex.err" ||
    fail "h1tex.cg listing compile failed"

# Python validation of containers and decoded ucode
"$PYTHON" - "$work" "$repo_root/tests/shader-compiler" <<'PY'
import sys, os, struct

work = sys.argv[1]
sys.path.append(sys.argv[2])
from ucode_decode import decode, groups

def load_container(path):
    b = open(path, "rb").read()
    if len(b) < 32:
        raise ValueError(f"{path}: container too small ({len(b)} bytes)")
    magic, rev, total_size, p_count, p_arr, prog, ucode_size, ucode_off = struct.unpack_from(">8I", b, 0)
    params = []
    for i in range(p_count):
        rec = p_arr + 48 * i
        typ, res, var, resIdx, nameoff, defVal, constoff, semoff, direction, paramno, isRef, isShared = struct.unpack_from(">12I", b, rec)
        name = b[nameoff:].split(bytes([0]))[0].decode("ascii", "replace") if 0 < nameoff < len(b) else ""
        sem = b[semoff:].split(bytes([0]))[0].decode("ascii", "replace") if 0 < semoff < len(b) else ""
        params.append({"type": typ, "res": res, "name": name, "sem": sem,
                       "constoff": constoff, "isRef": isRef})
    return {
        "magic": magic, "rev": rev, "total_size": total_size,
        "p_count": p_count, "params": params,
        "ucode_size": ucode_size, "ucode_off": ucode_off
    }

# Check 1: typed_tex
c1 = load_container(f"{work}/typed_tex.fpo")
if c1["magic"] != 0x00001b5c:
    sys.exit(f"FAIL typed_tex: bad magic 0x{c1['magic']:08x}")
types1 = [p["type"] for p in c1["params"]]
if 1066 not in types1:
    sys.exit("FAIL typed_tex: CG_SAMPLER2D (1066) not found in parameter table")
if 1069 not in types1:
    sys.exit("FAIL typed_tex: CG_SAMPLERCUBE (1069) not found in parameter table")
insns1 = list(decode(f"{work}/typed_tex.log"))
fetches1 = [insn for insn in insns1 if insn["opcode"] == 0x17]
if len(fetches1) != 3:
    sys.exit(f"FAIL typed_tex: expected 3 TEX (0x17) instructions, got {len(fetches1)}")

# A half-typed fetch is an fp16 DESTINATION, not fp16 arithmetic: the
# reference writes h2tex2D and h3texCUBE into the H bank with the
# destination-half bit set and the instruction still at prec=0, and the
# float-typed fetch beside them into R.  The two fields are independent
# (bit 7 is the bank, bits 22..23 the arithmetic precision), so a check on
# precision alone would read a fetch landing in the wrong bank as correct
# and hand the next instruction a register it does not name.  The register
# NUMBER is the allocator's business and is deliberately not pinned; the
# write mask is the fetch's own width and is.
WANT_BANKS = sorted([(0, 0xF), (1, 0x3), (1, 0x7)])

def bank_error(fetches):
    banks = sorted((insn["dsthalf"], insn["mask"]) for insn in fetches)
    if banks != WANT_BANKS:
        return (f"the three fetches write (half, mask) {banks}; expected "
                f"{WANT_BANKS} - f4tex2D into R with xyzw, h2tex2D into H "
                "with xy, h3texCUBE into H with xyz")
    for insn in fetches:
        if insn["prec"] != 0:
            return (f"a TEX carries arithmetic precision {insn['prec']}; a "
                    "fetch's precision is the texture unit's, so every one "
                    "of these is prec=0")
    return None

err = bank_error(fetches1)
if err:
    sys.exit("FAIL typed_tex: " + err)

# CONTROL: land the two half fetches in the float bank and change nothing
# else.  That is the defect this row is for - the value is then read from a
# register the consumer does not name - and it moves neither an opcode, a
# mask nor a precision, so everything else in this check still passes.
in_float_bank = [dict(insn, dsthalf=0) for insn in fetches1]
if bank_error(in_float_bank) is None:
    sys.exit("FAIL typed_tex: self-check - the bank check accepted three "
             "fetches that all wrote the float bank")

# Check 2: tex2d_f3
c2 = load_container(f"{work}/tex2d_f3.fpo")
if c2["magic"] != 0x00001b5c:
    sys.exit(f"FAIL tex2d_f3: bad magic 0x{c2['magic']:08x}")
types2 = [p["type"] for p in c2["params"]]
if 1047 not in types2: # CG_FLOAT3
    sys.exit("FAIL tex2d_f3: CG_FLOAT3 (1047) coordinate not found in parameter table")
if 1066 not in types2: # CG_SAMPLER2D
    sys.exit("FAIL tex2d_f3: CG_SAMPLER2D (1066) not found in parameter table")
insns2 = list(decode(f"{work}/tex2d_f3.log"))
if not any(insn["opcode"] == 0x17 and insn["mask"] == 0xF and insn["inputs"] >= 1 for insn in insns2):
    sys.exit("FAIL tex2d_f3: TEX instruction with full mask 0xF and input coord not found")

# Check 3: tex3d_bias
c3 = load_container(f"{work}/tex3d_bias.fpo")
types3 = [p["type"] for p in c3["params"]]
if 1067 not in types3: # CG_SAMPLER3D
    sys.exit("FAIL tex3d_bias: CG_SAMPLER3D (1067) not found in parameter table")
if 1066 not in types3: # CG_SAMPLER2D
    sys.exit("FAIL tex3d_bias: CG_SAMPLER2D (1066) not found in parameter table")
insns3 = list(decode(f"{work}/tex3d_bias.log"))
opcodes3 = set(insn["opcode"] for insn in insns3)
if 0x17 not in opcodes3:
    sys.exit("FAIL tex3d_bias: opcode TEX (0x17) not found for tex3D")
if 0x31 not in opcodes3:
    sys.exit("FAIL tex3d_bias: opcode TXB (0x31) not found for tex2Dbias")

# Each fetch must read the unit ITS OWN sampler was given.  Asserting only
# that both records exist and both opcodes appear passes a program that
# binds the samplers correctly and then samples the wrong textures - which
# is the class this composition broke in (the TXB fetch was invisible to
# the surviving-texture classifier, so its sampler got no unit at all).
# The ABSOLUTE numbers are deliberately not asserted: our allocator keeps
# relative declaration order for used implicit samplers and the reference
# numbers by first use, so this source is 3D=1/2D=0 here and 3D=0/2D=1
# there.  That gap is recorded (t_750d55be) and is not this row's subject.
#
# The records are pinned BY NAME, with their type, resource and live flag
# together (codex): a bare "1067 appears among the types" is a numeric word
# found somewhere in a container, and a matching word can be unrelated
# payload.  It also cannot see a second record of the same type, which is
# how the sampler3D case would hide a type-0 reflection record.
def named_record(container, name):
    hits = [p for p in container["params"] if p["name"] == name]
    if len(hits) != 1:
        sys.exit(f"FAIL tex3d_bias: expected exactly one parameter record "
                 f"named {name!r}, found {len(hits)}")
    return hits[0]

SAMPLER2D, SAMPLER3D, RES_TEXUNIT0 = 1066, 1067, 2048

def sampler_unit(container, name, cgtype):
    rec = named_record(container, name)
    if rec["type"] != cgtype:
        sys.exit(f"FAIL tex3d_bias: {name} has CGtype {rec['type']}, not "
                 f"{cgtype}")
    if not rec["isRef"]:
        sys.exit(f"FAIL tex3d_bias: {name} is bound but marked unreferenced, "
                 "so the fetch below reads a unit no runtime will fill")
    unit = rec["res"] - RES_TEXUNIT0
    if not 0 <= unit < 16:
        sys.exit(f"FAIL tex3d_bias: {name} carries resource {rec['res']}, "
                 f"which is not a texture unit (TEXUNIT0 is {RES_TEXUNIT0})")
    return unit

def association_error(insns, unit3d, unit2d):
    """None when each fetch reads the unit its own sampler record names."""
    tex_units = [i["texunit"] for i in insns if i["opcode"] == 0x17]
    txb_units = [i["texunit"] for i in insns if i["opcode"] == 0x31]
    if tex_units != [unit3d]:
        return (f"TEX reads unit(s) {tex_units}, but the sampler3D record is "
                f"bound to unit {unit3d}")
    if txb_units != [unit2d]:
        return (f"TXB reads unit(s) {txb_units}, but the sampler2D record is "
                f"bound to unit {unit2d}")
    return None

unit3d = sampler_unit(c3, "s3d", SAMPLER3D)
unit2d = sampler_unit(c3, "s2d", SAMPLER2D)
if unit3d == unit2d:
    sys.exit(f"FAIL tex3d_bias: both samplers bound to unit {unit3d}; "
             "the association assertion below would be vacuous")
err = association_error(insns3, unit3d, unit2d)
if err:
    sys.exit("FAIL tex3d_bias: " + err)

# CONTROL (codex): swap the two fetches' unit fields and leave the named
# reflection untouched - the program now samples the 3D texture through the
# 2D sampler's unit and back, which is the defect this row exists for.  The
# checker must reject it.  A checker that only counted opcodes, or read the
# units out of the same records it compares them against, passes this.
swapped = [dict(i) for i in insns3]
for i in swapped:
    if i["opcode"] == 0x17:
        i["texunit"] = unit2d
    elif i["opcode"] == 0x31:
        i["texunit"] = unit3d
if association_error(swapped, unit3d, unit2d) is None:
    sys.exit("FAIL tex3d_bias: self-check - the association check accepted a "
             "program whose two fetches had their texture units swapped")

# Check 3b: a dead tex2Dbias goes away, and #pragma alphakill keeps it.
# These two shaders differ by one pragma line.  Both rows used to compile
# their shader and assert nothing about it, which is the same as not
# running: elimination and preservation are opposite behaviours and a
# compiler that got either backwards passed.  The alphakill row matters
# because the fetch it preserves has no readers - it is kept for the pixel
# kill its texture drives, so ordinary dead-code elimination is exactly
# what must NOT happen to it.
c3b = load_container(f"{work}/dead_txb.fpo")
insns3b = list(decode(f"{work}/dead_txb.log"))
if any(i["opcode"] == 0x31 for i in insns3b):
    sys.exit("FAIL dead_txb: TXB (0x31) survives although its result is "
             "unread and no #pragma alphakill names its sampler")
live3b = [i for i in insns3b if i["opcode"] == 0x17]
if len(live3b) != 1:
    sys.exit(f"FAIL dead_txb: expected the one surviving tex2D fetch, got "
             f"{len(live3b)} TEX instructions")
s2d_dead = named_record(c3b, "s2d")
dead_rec = named_record(c3b, "s_dead")
if not s2d_dead["isRef"] or s2d_dead["res"] != RES_TEXUNIT0:
    sys.exit(f"FAIL dead_txb: the live sampler s2d is res={s2d_dead['res']} "
             f"isRef={s2d_dead['isRef']}; expected TEXUNIT0 ({RES_TEXUNIT0}) "
             "and referenced")
if dead_rec["isRef"]:
    sys.exit("FAIL dead_txb: s_dead is marked referenced although its only "
             "fetch was eliminated")
if RES_TEXUNIT0 <= dead_rec["res"] < RES_TEXUNIT0 + 16:
    sys.exit(f"FAIL dead_txb: the eliminated sampler s_dead still holds "
             f"texture unit {dead_rec['res'] - RES_TEXUNIT0}, which the "
             "runtime would then be asked to fill")
if live3b[0]["texunit"] != s2d_dead["res"] - RES_TEXUNIT0:
    sys.exit(f"FAIL dead_txb: the surviving TEX reads unit "
             f"{live3b[0]['texunit']}, not s2d's "
             f"{s2d_dead['res'] - RES_TEXUNIT0}")

c3c = load_container(f"{work}/dead_txb_ak.fpo")
insns3c = list(decode(f"{work}/dead_txb_ak.log"))
kept = [i for i in insns3c if i["opcode"] == 0x31]
if len(kept) != 1:
    sys.exit(f"FAIL dead_txb_ak: expected the alphakill fetch to be kept as "
             f"one TXB, got {len(kept)}")
tex3c = [i for i in insns3c if i["opcode"] == 0x17]
if len(tex3c) != 1:
    sys.exit(f"FAIL dead_txb_ak: expected one surviving tex2D fetch, got "
             f"{len(tex3c)} TEX instructions")
s2d_ak = named_record(c3c, "s2d")
kill_ak = named_record(c3c, "s_dead")
if not kill_ak["isRef"]:
    sys.exit("FAIL dead_txb_ak: the alphakill sampler is marked unreferenced "
             "although its fetch was kept")
unit_ak = kill_ak["res"] - RES_TEXUNIT0
unit_live = s2d_ak["res"] - RES_TEXUNIT0
if not 0 <= unit_ak < 16 or unit_ak == unit_live:
    sys.exit(f"FAIL dead_txb_ak: the alphakill sampler holds res "
             f"{kill_ak['res']} beside s2d's {s2d_ak['res']}; the two fetches "
             "need distinct texture units for the check below to mean "
             "anything")
if kept[0]["texunit"] != unit_ak or tex3c[0]["texunit"] != unit_live:
    sys.exit(f"FAIL dead_txb_ak: TXB reads unit {kept[0]['texunit']} and TEX "
             f"reads {tex3c[0]['texunit']}; their own records name "
             f"{unit_ak} and {unit_live}")
if not any(p["name"].startswith("$kill_") for p in c3c["params"]):
    sys.exit("FAIL dead_txb_ak: no synthetic $kill_NNNN parameter for the "
             "#pragma alphakill sampler")

# Check 4: lit intrinsic hardware LITEX2 (0x3C) and zero-exponent optimization
c4 = load_container(f"{work}/lit.fpo")
insns4 = list(decode(f"{work}/lit.log"))
opcodes4 = set(insn["opcode"] for insn in insns4)
if 0x3C not in opcodes4:
    sys.exit("FAIL lit: hardware opcode LITEX2 (0x3C) not emitted")
if 0x1D not in opcodes4:
    sys.exit("FAIL lit: LG2 (0x1D) opcode not found for general lit exponent")

c4_zero = load_container(f"{work}/lit_zero.fpo")
insns4_zero = list(decode(f"{work}/lit_zero.log"))
opcodes4_zero = set(insn["opcode"] for insn in insns4_zero)
if 0x3C not in opcodes4_zero:
    sys.exit("FAIL lit_zero: hardware opcode LITEX2 (0x3C) not emitted")
if 0x1D in opcodes4_zero:
    sys.exit("FAIL lit_zero: LG2 (0x1D) opcode emitted for zero exponent (expected NaN-free fast path)")
if len(insns4_zero) > 4:
    sys.exit(f"FAIL lit_zero: expected at most 4 instructions (3 arithmetic + export fold), got {len(insns4_zero)}")

# The reference's zero-exponent program is MOV R0.x, TEX0; MAX R0.y, R0.x,
# 0; LITEX2 R0, R0.xyzz - it NEVER READS the ndoth lane, so its result
# cannot depend on that argument's sign (codex, from his own decode of the
# reference).  Ours read it: the earlier candidate gated z on ndoth > 0
# with SGT/MUL/MAD.  Counting opcodes cannot see that; the operand
# swizzles can, so this asserts which lanes of the varying the program
# actually consumes.
def varying_lanes_read(rows):
    lanes, i = set(), 0
    while i < len(rows):
        w = rows[i]
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == 2)   # CONST
        mask = (w[0] >> 9) & 0xF
        for s in (1, 2, 3):
            if (w[s] & 3) == 1:                                 # INPUT
                for lane in range(4):
                    if mask & (1 << lane):
                        lanes.add((w[s] >> (9 + 2 * lane)) & 3)
        i += 1 + (1 if consts else 0)
    return lanes

zero_rows = groups(f"{work}/lit_zero.log")
zero_lanes = varying_lanes_read(zero_rows)
if zero_lanes != {0}:
    sys.exit(f"FAIL lit_zero: the program reads varying lanes "
             f"{sorted(zero_lanes)}; with a zero exponent the reference reads "
             "only lane 0 (ndotl), so a read of lane 1 is the ndoth gating "
             "the hardware LITEX2 does not do")

# CONTROL: point one input read at lane 1 and leave everything else alone -
# the ndoth read, spelled as a swizzle rather than as an extra instruction.
# An opcode census cannot see this edit at all; if the walk above cannot
# either, it is asserting nothing about which argument the program consumes.
doctored, patched = [list(r) for r in zero_rows], False
for row in doctored:
    for s in (1, 2, 3):
        if (row[s] & 3) == 1 and not patched:
            row[s] = (row[s] & ~(3 << 9)) | (1 << 9)            # lane x -> y
            patched = True
if not patched:
    sys.exit("FAIL lit_zero: self-check - no input read to doctor, so the "
             "lane assertion above ran against a program that reads no "
             "varying at all")
if varying_lanes_read(doctored) == {0}:
    sys.exit("FAIL lit_zero: self-check - the lane walk still reports only "
             "lane 0 after an input operand was repointed at lane 1")

# The hold this row closes was a SHAPE, not an opcode: the candidate spelled
# lit with SGT/MUL/MAD gating in seventeen instructions while the reference
# spelled it in six.  LITEX2 being present says nothing about that, because
# the gating sat around it.  The bound is the reference's six plus our
# standing export-fold MOV, with slack for the half conversions this
# fixture's half3 varyings need - it is a regression bound, not a claim of
# byte parity.
if len(insns4) > 10:
    sys.exit(f"FAIL lit: {len(insns4)} instructions for one lit(); the "
             "reference emits six, and seventeen was the gated shape this "
             "row exists to keep out")

# Check 5: transpose in VP
c5 = load_container(f"{work}/transpose_vp.vpo")
if c5["magic"] != 0x00001b5b:
    sys.exit(f"FAIL transpose_vp: bad VP magic 0x{c5['magic']:08x} (expected 0x00001b5b)")
types5 = [p["type"] for p in c5["params"]]
if 1064 not in types5: # CG_FLOAT4x4
    sys.exit("FAIL transpose_vp: CG_FLOAT4x4 (1064) not found in parameter table")
if c5["ucode_size"] == 0:
    sys.exit("FAIL transpose_vp: empty ucode in VP container")
c5_double = load_container(f"{work}/transpose_vp_double.vpo")
if c5_double["ucode_size"] == 0:
    sys.exit("FAIL transpose_vp_double: empty ucode in VP double-transpose container")

# Check 6: transpose in FP (constructed, element indexing, and uniform float3x3)
c6 = load_container(f"{work}/transpose_fp.fpo")
if c6["magic"] != 0x00001b5c:
    sys.exit(f"FAIL transpose_fp: bad FP magic 0x{c6['magic']:08x}")
if c6["ucode_size"] == 0:
    sys.exit("FAIL transpose_fp: empty ucode in FP container")

c6_elem = load_container(f"{work}/transpose_fp_elem.fpo")
if c6_elem["ucode_size"] == 0:
    sys.exit("FAIL transpose_fp_elem: empty ucode in FP element check container")

c6_u = load_container(f"{work}/transpose_fp_uniform.fpo")
if c6_u["magic"] != 0x00001b5c:
    sys.exit(f"FAIL transpose_fp_uniform: bad FP magic 0x{c6_u['magic']:08x}")
u_types = [p["type"] for p in c6_u["params"]]
u_names = [p["name"] for p in c6_u["params"]]
if 1059 not in u_types: # CG_FLOAT3x3
    sys.exit("FAIL transpose_fp_uniform: CG_FLOAT3x3 (1059) parent not found in parameter table")
if "m[0]" not in u_names or "m[1]" not in u_names or "m[2]" not in u_names:
    sys.exit(f"FAIL transpose_fp_uniform: row records m[0], m[1], m[2] not found: {u_names}")

# Check 7: half-texture precision boundary
insns7 = list(decode(f"{work}/h1tex.log"))
if len(insns7) < 2:
    sys.exit(f"FAIL h1tex: expected at least 2 instructions, got {len(insns7)}")
tex_insn = insns7[0]
if tex_insn["opcode"] != 0x17 or tex_insn["prec"] != 0:
    sys.exit(f"FAIL h1tex: instruction 0 expected TEX (0x17) with prec=0, got {tex_insn}")
# Second instruction is precision cast to float at prec=1
cast_insn = insns7[1]
if cast_insn["opcode"] != 0x01 or cast_insn["prec"] != 1:
    sys.exit(f"FAIL h1tex: instruction 1 expected MOV (0x01) with prec=1, got {cast_insn}")

PY

# 8. tex2Dbias in VP refusal check (negative test)
cat > "$work/tex2dbias_vp.cg" << 'EOF'
float4 main(float4 pos : POSITION,
            uniform sampler2D s) : POSITION
{
    float4 val = tex2Dbias(s, pos);
    return pos + val;
}
EOF

# The refusal is checked through a function so the SAME check can be run
# against a compiler that accepts everything.  An earlier version of this
# script reported a missing compiler as "typed_tex.cg failed to compile",
# which is the accept-all stub's problem in reverse: a row that cannot
# distinguish the thing it is asserting from the harness being broken.
check_vp_refusal() {                       # $1 compiler, $2 label
    local cc="$1" label="$2" rc=0
    rm -f "$work/tex2dbias_vp.vpo" "$work/vp_err.log"
    "$cc" -p sce_vp_rsx --emit-container "$work/tex2dbias_vp.vpo" \
        "$work/tex2dbias_vp.cg" > /dev/null 2> "$work/vp_err.log" || rc=$?
    refusal_status "$rc" "$label"
    [[ "$rc" -eq 1 ]] || fail "$label: tex2dbias in VP exited $rc, expected 1"
    [[ ! -e "$work/tex2dbias_vp.vpo" ]] ||
        fail "$label: tex2dbias in VP created a container on refusal"
    grep -q -E "vertex texture fetch \(tex2Dbias\) is not supported in VP; refusing" \
        "$work/vp_err.log" ||
        fail "$label: tex2dbias in VP refused without the named diagnostic"
}

check_vp_refusal "$compiler" "tex2dbias_vp"

# CONTROL: the refusal check must reject a compiler that accepts everything
# and writes a container anyway.  Without this the row above passes on any
# binary that refuses for any reason at all - including one that cannot run.
stub="$work/accept-all.sh"
printf '#!/usr/bin/env bash\nout=""\nwhile [[ $# -gt 0 ]]; do [[ $1 == --emit-container ]] && out=$2; shift; done\n[[ -n "$out" ]] && printf x > "$out"\nexit 0\n' > "$stub"
chmod +x "$stub"
if ( check_vp_refusal "$stub" "accept-all stub" ) > /dev/null 2>&1; then
    fail "self-check: the VP refusal row passed against an accept-all stub"
fi
rm -f "$work/tex2dbias_vp.vpo"
check_vp_refusal "$compiler" "tex2dbias_vp (after the stub control)"

echo "stdlib-bucket-c-test: PASS"
