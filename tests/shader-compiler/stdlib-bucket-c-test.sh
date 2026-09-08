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
compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
if [ ! -x "$compiler" ] && [ -f "${compiler}.exe" ]; then
    compiler="${compiler}.exe"
fi
[[ -x "$compiler" ]] || { echo "FAIL: rsx-cg-compiler not executable: $compiler" >&2; exit 1; }

PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python

work="${TMPDIR:-/tmp}/ps3dk-bucket-c-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

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
from ucode_decode import decode

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
        params.append({"type": typ, "res": res, "name": name, "sem": sem, "constoff": constoff})
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
tex_count1 = sum(1 for insn in insns1 if insn["opcode"] == 0x17)
if tex_count1 < 3:
    sys.exit(f"FAIL typed_tex: expected at least 3 TEX (0x17) instructions, got {tex_count1}")

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

vp_rc=0
"$compiler" -p sce_vp_rsx --emit-container "$work/tex2dbias_vp.vpo" "$work/tex2dbias_vp.cg" > /dev/null 2> "$work/vp_err.log" || vp_rc=$?

[[ "$vp_rc" -eq 1 ]] || fail "tex2dbias in VP unexpectedly succeeded (exit code $vp_rc, expected 1)"
[[ ! -e "$work/tex2dbias_vp.vpo" ]] || fail "tex2dbias in VP created container on refusal"
if ! grep -q -E "vertex texture fetch \(tex2Dbias\) is not supported in VP; refusing" "$work/vp_err.log"; then
    cat "$work/vp_err.log" >&2
    fail "tex2dbias in VP failed without expected texture fetch refusal diagnostic"
fi

echo "stdlib-bucket-c-test: PASS"
