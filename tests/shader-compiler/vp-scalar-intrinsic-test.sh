#!/usr/bin/env bash
# t_542450b2: VP scalar intrinsic lowering (sin, cos, clamp, sqrt, tan).
#
# Deliverable: close the 7 SDK rows previously failing with
#   'nv40-general: VP scalar intrinsic lowering deferred'
#
# Shape & Container Breakdown (vs Reference sce-cgc 475 -p sce_vp_rsx):
#   - Unit clamp lowering:
#       Our compiler emits standalone MOV_sat (saturate bit set on MOV).
#       The reference also uses standalone MOV_sat for the input-only fixture;
#       vs_procAnim folds saturate into its arithmetic producer (t_c67a9c41).
#   - Container deltas measured:
#       * vs_procAnim.cg:
#           Reference: 1728 B (params: 1008 B, strings: 416 B, consts: 32 B, ucode: 240 B [15 instrs])
#           Ours:      1920 B (params: 1008 B, strings: 416 B, consts: 32 B, ucode: 432 B [27 instrs])
#           Delta: +192 B total, 100% attributed to ucode (+12 instructions).
#       * point_sprite_shader_vertex.cg (all 6 ParticleSimulator rows):
#           Reference: 1456 B (params: 768 B, strings: 384 B, consts: 32 B, ucode: 240 B [15 instrs])
#           Ours:      1600 B (params: 768 B, strings: 400 B, consts: 32 B, ucode: 368 B [23 instrs])
#           Delta: +144 B total (+128 B ucode = +8 instructions, +16 B string/defaults alignment).
#
# Reference Oracle Sequences & Probes Decoded:
#   - clamp(p, 0.0, 1.0) -> MOV_SAT o[COL0], v[ATTR0]
#   - clamp(p, lo, hi) / reversed clamp(p, 1.0, 0.0):
#       MIN r0, v[ATTR0], c[467].x;  MAX o[COL0], r0, c[467].y (MIN against maxVal first, then MAX against minVal)
#   - sqrt(p.x) / variable sqrt(u_denorm) / non-positive literals sqrt(0.0), sqrt(-1.0):
#       emits runtime RSQ r0.x, ...;  RCP o[COL0], r0.x
#   - positive literals sqrt(4.0), sqrt(1.0), sqrt(1.4e-45f):
#       reference constant-folds to #const C[467] (e.g. 0x1a3504f3 for 1.4e-45f) and emits MOV o[COL0], c[467].x
#   - tan(p.x):
#       COS r0.y, v.x;  SIN r0.x, v.x;  RCP r0.y, r0.y;  MUL o[COL0], r0.x, r0.y
#
# Full Container Twin Spellings Asserted 100% Byte-Identical:
#   - clamp(p, 0.0f, 1.0f) == saturate(p) (368 B full container)
#   - clamp(p, u.x, u.y)   == max(min(p, u.y), u.x) (432 B full container)
#   - sqrt(p)              == 1.0f / rsqrt(p) (400 B full container)
#   - sqrt(4.0f)           == 2.0f (368 B full container)
#   - sin(0.0f)            == 0.0f (368 B full container)
#   - cos(0.0f)            == 1.0f (368 B full container)
#   - tan(0.0f)            == 0.0f (368 B full container)
# Controls Asserted Runtime RSQ + RCP:
#   - sqrt(0.0f)
#   - sqrt(-1.0f)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-$root/tools/rsx-cg-compiler/build/rsx-cg-compiler}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "compiler not executable: $compiler"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

shaders_dir="$root/tools/rsx-cg-compiler/tests/shaders"

fixtures=(
    "vp_scalar_sin_cos_v"
    "vp_scalar_vector_sin_v"
    "vp_scalar_vector_cos_v"
    "vp_scalar_clamp_sat_v"
    "vp_scalar_saturate_v"
    "vp_scalar_clamp_bounds_v"
    "vp_scalar_clamp_minmax_v"
    "vp_scalar_sqrt_v"
    "vp_scalar_sqrt_twin_v"
    "vp_scalar_sqrt_lit_v"
    "vp_scalar_sqrt_lit_twin_v"
    "vp_scalar_sqrt_zero_v"
    "vp_scalar_sqrt_neg_v"
    "vp_scalar_sin_lit_v"
    "vp_scalar_sin_lit_twin_v"
    "vp_scalar_cos_lit_v"
    "vp_scalar_cos_lit_twin_v"
    "vp_scalar_tan_lit_v"
    "vp_scalar_tan_lit_twin_v"
    "vp_scalar_tan_v"
)

for name in "${fixtures[@]}"; do
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" -p sce_vp_rsx             --emit-container "$work/$name.vpo" "$shaders_dir/$name.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    if [[ "$rc" != 0 ]]; then
        tail -n 8 "$work/$name.log" >&2
        fail "$name: VP scalar intrinsic fixture did not compile (exit $rc)"
    fi
    [[ -s "$work/$name.vpo" ]] || fail "$name: compiler wrote no container"
done

python3 - "$work" <<'PY'
import pathlib
import struct
import sys

work = pathlib.Path(sys.argv[1])

def read_container(name):
    p = work / f"{name}.vpo"
    if not p.exists() or p.stat().st_size < 32:
        raise SystemExit(f"FAIL: {name} container missing or too short")
    return p.read_bytes()

def read_raw_ucode(name):
    data = read_container(name)
    _, _, _, _, _, _, size, off = struct.unpack_from(">8I", data, 0)
    if size == 0 or size % 16 != 0 or off + size > len(data):
        raise SystemExit(f"FAIL: {name} invalid ucode extent [{off}, {off+size}] (len {len(data)})")
    return data[off:off+size]

def read_ucode(name):
    raw = read_raw_ucode(name)
    return [struct.unpack_from(">4I", raw, i * 16) for i in range(len(raw) // 16)]

# 1. Scalar sin and cos: must contain both SIN (sop=15) and COS (sop=16)
#    reading counter (INPUT ATTR8, swizzle x)
words_sc = read_ucode("vp_scalar_sin_cos_v")
sin_sc = []
cos_sc = []
for i, w in enumerate(words_sc):
    sop = (w[1] >> 27) & 0x1F
    if sop in (15, 16):
        in_src = (w[1] >> 8) & 0x0F
        sr = (((w[2] >> 0) & 0x3F) << 11) | ((w[3] >> 21) & 0x7FF)
        sr_type = sr & 3
        swz_x = (sr >> 14) & 3
        if sop == 15:
            sin_sc.append((i, sr_type, in_src, swz_x))
        else:
            cos_sc.append((i, sr_type, in_src, swz_x))

if not sin_sc or not cos_sc:
    sops_sc = [(w[1] >> 27) & 0x1F for w in words_sc]
    raise SystemExit(f"FAIL: vp_scalar_sin_cos_v expected both SIN(15) and COS(16), got sops {sops_sc}")

for idx, sr_type, in_src, swz in sin_sc:
    if sr_type != 2 or in_src != 8 or swz != 0:
        raise SystemExit(f"FAIL: vp_scalar_sin_cos_v SIN instr {idx} source is not counter/ATTR8.x (type={sr_type}, in_src={in_src}, swz={swz})")
for idx, sr_type, in_src, swz in cos_sc:
    if sr_type != 2 or in_src != 8 or swz != 0:
        raise SystemExit(f"FAIL: vp_scalar_sin_cos_v COS instr {idx} source is not counter/ATTR8.x (type={sr_type}, in_src={in_src}, swz={swz})")

# 2. Vector sin: must emit 4 scalar SIN ops (sop=15), each with single-lane mask,
#    and the source swizzle must select the corresponding input lane from val (INPUT ATTR8),
#    writing to oResult (TC0=7).
words_sin = read_ucode("vp_scalar_vector_sin_v")
sin_ops = []
for i, w in enumerate(words_sin):
    sop = (w[1] >> 27) & 0x1F
    if sop == 15:
        smask = (w[3] >> 17) & 0xF
        dest = (w[3] >> 2) & 0x1F
        sca_result = (w[3] >> 12) & 1
        in_src = (w[1] >> 8) & 0x0F
        sr = (((w[2] >> 0) & 0x3F) << 11) | ((w[3] >> 21) & 0x7FF)
        sr_type = sr & 3
        swz_x = (sr >> 14) & 0x3
        sin_ops.append((i, smask, swz_x, sr_type, in_src, dest, sca_result))

if len(sin_ops) != 4:
    raise SystemExit(f"FAIL: vector sin expected 4 SIN ops, got {len(sin_ops)}")
masks_sin = [smask for _, smask, _, _, _, _, _ in sin_ops]
if sorted(masks_sin) != [1, 2, 4, 8]:
    raise SystemExit(f"FAIL: vector sin masks {masks_sin}, expected [1, 2, 4, 8]")

mask_to_lane = {8: 0, 4: 1, 2: 2, 1: 3}
for idx, smask, swz, sr_type, in_src, dest, sca_result in sin_ops:
    expected_lane = mask_to_lane[smask]
    if swz != expected_lane:
        raise SystemExit(f"FAIL: vector sin instr {idx} mask {smask} selects source lane {swz}, expected {expected_lane}")
    if sr_type != 2 or in_src != 8:
        raise SystemExit(f"FAIL: vector sin instr {idx} source is not val/ATTR8 (type={sr_type}, in_src={in_src})")
    if dest != 7:
        raise SystemExit(f"FAIL: vector sin instr {idx} dest {dest} is not oResult/TC0(7)")
    if not sca_result:
        raise SystemExit(f"FAIL: vector sin instr {idx} does not have NV40_VP_INST_SCA_RESULT (w[3] bit 12) set")

# 3. Vector cos: must emit 4 scalar COS ops (sop=16), each with single-lane mask and matching lane from val (INPUT ATTR8),
#    writing to oResult (TC0=7) with NV40_VP_INST_SCA_RESULT set.
words_cos = read_ucode("vp_scalar_vector_cos_v")
cos_ops = []
for i, w in enumerate(words_cos):
    sop = (w[1] >> 27) & 0x1F
    if sop == 16:
        smask = (w[3] >> 17) & 0xF
        dest = (w[3] >> 2) & 0x1F
        sca_result = (w[3] >> 12) & 1
        in_src = (w[1] >> 8) & 0x0F
        sr = (((w[2] >> 0) & 0x3F) << 11) | ((w[3] >> 21) & 0x7FF)
        sr_type = sr & 3
        swz_x = (sr >> 14) & 0x3
        cos_ops.append((i, smask, swz_x, sr_type, in_src, dest, sca_result))

if len(cos_ops) != 4:
    raise SystemExit(f"FAIL: vector cos expected 4 COS ops, got {len(cos_ops)}")
masks_cos = [smask for _, smask, _, _, _, _, _ in cos_ops]
if sorted(masks_cos) != [1, 2, 4, 8]:
    raise SystemExit(f"FAIL: vector cos masks {masks_cos}, expected [1, 2, 4, 8]")
for idx, smask, swz, sr_type, in_src, dest, sca_result in cos_ops:
    expected_lane = mask_to_lane[smask]
    if swz != expected_lane:
        raise SystemExit(f"FAIL: vector cos instr {idx} mask {smask} selects source lane {swz}, expected {expected_lane}")
    if sr_type != 2 or in_src != 8:
        raise SystemExit(f"FAIL: vector cos instr {idx} source is not val/ATTR8 (type={sr_type}, in_src={in_src})")
    if dest != 7:
        raise SystemExit(f"FAIL: vector cos instr {idx} dest {dest} is not oResult/TC0(7)")
    if not sca_result:
        raise SystemExit(f"FAIL: vector cos instr {idx} does not have NV40_VP_INST_SCA_RESULT (w[3] bit 12) set")

# 4. Scalar clamp sat: require MOV instruction with saturate bit set writing to oResult (TC0=7)
#    from val (INPUT ATTR8) with XYZW swizzle
words_clamp_sat = read_ucode("vp_scalar_clamp_sat_v")
sat_movs = []
for w in words_clamp_sat:
    is_sat = (w[0] >> 26) & 1
    is_vec_res = (w[0] >> 30) & 1
    vec_op = (w[1] >> 22) & 0x1F
    vec_mask = (w[3] >> 13) & 0xF
    dest = (w[3] >> 2) & 0x1F
    input_src = (w[1] >> 8) & 0x0F
    sr0 = (((w[1] >> 0) & 0xFF) << 9) | ((w[2] >> 23) & 0x1FF)
    sr0_type = sr0 & 3
    sr0_swz = tuple((sr0 >> (14 - 2 * k)) & 3 for k in range(4))
    if (is_sat and is_vec_res and vec_op == 1 and vec_mask == 0xF and
        dest == 7 and input_src == 8 and sr0_type == 2 and sr0_swz == (0, 1, 2, 3)):
        sat_movs.append(w)

if not sat_movs:
    raise SystemExit("FAIL: clamp_sat has no MOV_sat instruction writing oResult(TC0=7) with full mask from val(INPUT=8, swz=xyzw)")

# Full container comparison: clamp(x, 0, 1) vs saturate(x)
cnt_clamp_sat = read_container("vp_scalar_clamp_sat_v")
cnt_sat = read_container("vp_scalar_saturate_v")
if cnt_clamp_sat != cnt_sat:
    raise SystemExit(f"FAIL: full container clamp(x, 0, 1) ({len(cnt_clamp_sat)} B) != saturate(x) ({len(cnt_sat)} B)")

# 5. Clamp bounds: must emit MIN (vop=9) then MAX (vop=10) with operand dependency
words_clamp_bounds = read_ucode("vp_scalar_clamp_bounds_v")
min_instrs = [(i, w) for i, w in enumerate(words_clamp_bounds) if ((w[1] >> 22) & 0x1F) == 9]
max_instrs = [(i, w) for i, w in enumerate(words_clamp_bounds) if ((w[1] >> 22) & 0x1F) == 10]
if not min_instrs or not max_instrs:
    raise SystemExit("FAIL: clamp_bounds expected MIN(9) and MAX(10)")
min_idx, min_w = min_instrs[0]
max_idx, max_w = max_instrs[0]
if min_idx > max_idx:
    raise SystemExit(f"FAIL: clamp_bounds emitted MAX at {max_idx} before MIN at {min_idx}; expected MIN then MAX")

# Operand dependency: MIN consumes upper bound u_bounds.y
sr1_min = (min_w[2] >> 6) & 0x1FFFF
min_const_src = (min_w[1] >> 12) & 0x1FF
min_src1_type = sr1_min & 3
min_src1_swz = tuple((sr1_min >> (14 - 2 * k)) & 3 for k in range(4))
if min_src1_type != 3 or min_const_src != 467 or min_src1_swz != (1, 1, 1, 1):
    raise SystemExit(f"FAIL: clamp_bounds MIN does not consume upper bound u_bounds.y (type={min_src1_type}, const_src={min_const_src}, swz={min_src1_swz})")

# Operand dependency: MAX must consume the temporary produced by MIN and lower bound u_bounds.x
min_dst = (min_w[0] >> 15) & 0x3F
sr0_max = (((max_w[1] >> 0) & 0xFF) << 9) | ((max_w[2] >> 23) & 0x1FF)
max_src0_type = sr0_max & 3
max_src0_reg = (sr0_max >> 2) & 0x1F
if max_src0_type != 1 or max_src0_reg != min_dst:
    raise SystemExit(f"FAIL: clamp_bounds MAX does not consume MIN temp {min_dst} (src0_type={max_src0_type}, reg={max_src0_reg})")

sr1_max = (max_w[2] >> 6) & 0x1FFFF
max_const_src = (max_w[1] >> 12) & 0x1FF
max_src1_type = sr1_max & 3
max_src1_swz = tuple((sr1_max >> (14 - 2 * k)) & 3 for k in range(4))
if max_src1_type != 3 or max_const_src != 467 or max_src1_swz != (0, 0, 0, 0):
    raise SystemExit(f"FAIL: clamp_bounds MAX does not consume lower bound u_bounds.x (type={max_src1_type}, const_src={max_const_src}, swz={max_src1_swz})")

# Full container comparison: clamp(x, lo, hi) vs max(min(x, hi), lo)
cnt_clamp_bounds = read_container("vp_scalar_clamp_bounds_v")
cnt_minmax = read_container("vp_scalar_clamp_minmax_v")
if cnt_clamp_bounds != cnt_minmax:
    raise SystemExit(f"FAIL: full container clamp(x, lo, hi) ({len(cnt_clamp_bounds)} B) != max(min(x, hi), lo) ({len(cnt_minmax)} B)")

# 6. Sqrt: must emit RSQ (sop=4) then RCP (sop=2) with operand dependency
words_sqrt = read_ucode("vp_scalar_sqrt_v")
rsq_instrs = [(i, w) for i, w in enumerate(words_sqrt) if ((w[1] >> 27) & 0x1F) == 4]
rcp_instrs = [(i, w) for i, w in enumerate(words_sqrt) if ((w[1] >> 27) & 0x1F) == 2]
if not rsq_instrs or not rcp_instrs:
    raise SystemExit("FAIL: sqrt expected RSQ(4) and RCP(2)")
rsq_idx, rsq_w = rsq_instrs[0]
rcp_idx, rcp_w = rcp_instrs[0]
if rsq_idx > rcp_idx:
    raise SystemExit(f"FAIL: sqrt emitted RCP at {rcp_idx} before RSQ at {rsq_idx}; expected RSQ then RCP")

rsq_mask = (rsq_w[3] >> 17) & 0xF
if rsq_mask not in mask_to_lane:
    raise SystemExit(f"FAIL: sqrt RSQ writemask {rsq_mask} is not a single lane mask")
rsq_lane = mask_to_lane[rsq_mask]
rsq_dst = (rsq_w[3] >> 7) & 0x3F

sr2_rcp = (((rcp_w[2] >> 0) & 0x3F) << 11) | ((rcp_w[3] >> 21) & 0x7FF)
rcp_src_type = sr2_rcp & 3
rcp_src_reg = (sr2_rcp >> 2) & 0x1F
rcp_src_swz = (sr2_rcp >> 14) & 3
if rcp_src_type != 1 or rcp_src_reg != rsq_dst:
    raise SystemExit(f"FAIL: sqrt RCP does not consume RSQ dst temp {rsq_dst} (src_type={rcp_src_type}, reg={rcp_src_reg})")
if rcp_src_swz != rsq_lane:
    raise SystemExit(f"FAIL: sqrt RCP swizzle {rcp_src_swz} does not select RSQ written lane {rsq_lane}")

# Full container comparison: sqrt(x) vs 1.0f / rsqrt(x)
cnt_sqrt = read_container("vp_scalar_sqrt_v")
cnt_sqrt_twin = read_container("vp_scalar_sqrt_twin_v")
if cnt_sqrt != cnt_sqrt_twin:
    raise SystemExit(f"FAIL: full container sqrt(x) ({len(cnt_sqrt)} B) != 1.0f / rsqrt(x) ({len(cnt_sqrt_twin)} B)")

# Full container comparison: positive literal sqrt(4.0f) vs literal 2.0f twin
cnt_sqrt_lit = read_container("vp_scalar_sqrt_lit_v")
cnt_sqrt_lit_twin = read_container("vp_scalar_sqrt_lit_twin_v")
if cnt_sqrt_lit != cnt_sqrt_lit_twin:
    raise SystemExit(f"FAIL: full container sqrt(4.0f) ({len(cnt_sqrt_lit)} B) != 2.0f ({len(cnt_sqrt_lit_twin)} B)")

# Assert positive literal sqrt has NO RSQ and NO RCP (strictly folded into constant)
words_sqrt_lit = read_ucode("vp_scalar_sqrt_lit_v")
rsq_lit = [w for w in words_sqrt_lit if ((w[1] >> 27) & 0x1F) == 4]
rcp_lit = [w for w in words_sqrt_lit if ((w[1] >> 27) & 0x1F) == 2]
if rsq_lit or rcp_lit:
    raise SystemExit("FAIL: sqrt(4.0f) emitted runtime RSQ or RCP; expected constant fold")

# Assert non-positive literal controls sqrt(0.0f) and sqrt(-1.0f) emit runtime RSQ and RCP
for ctl_name in ("vp_scalar_sqrt_zero_v", "vp_scalar_sqrt_neg_v"):
    words_ctl = read_ucode(ctl_name)
    rsq_ctl = [w for w in words_ctl if ((w[1] >> 27) & 0x1F) == 4]
    rcp_ctl = [w for w in words_ctl if ((w[1] >> 27) & 0x1F) == 2]
    if not rsq_ctl or not rcp_ctl:
        raise SystemExit(f"FAIL: {ctl_name} expected runtime RSQ(4) and RCP(2)")

# Full container comparison: literal sin/cos/tan vs literal twins
for op_name in ("sin", "cos", "tan"):
    cnt_op = read_container(f"vp_scalar_{op_name}_lit_v")
    cnt_op_twin = read_container(f"vp_scalar_{op_name}_lit_twin_v")
    if cnt_op != cnt_op_twin:
        raise SystemExit(f"FAIL: full container {op_name}(0.0f) ({len(cnt_op)} B) != twin ({len(cnt_op_twin)} B)")

# 7. Tan: operand dependencies (RCP consumes COS, MUL consumes SIN and RCP)
words_tan = read_ucode("vp_scalar_tan_v")
cos_instrs = [(i, w) for i, w in enumerate(words_tan) if ((w[1] >> 27) & 0x1F) == 16]
sin_instrs = [(i, w) for i, w in enumerate(words_tan) if ((w[1] >> 27) & 0x1F) == 15]
rcp_tan_instrs = [(i, w) for i, w in enumerate(words_tan) if ((w[1] >> 27) & 0x1F) == 2]
mul_tan_instrs = [(i, w) for i, w in enumerate(words_tan) if ((w[1] >> 22) & 0x1F) == 2]
if not cos_instrs or not sin_instrs or not rcp_tan_instrs or not mul_tan_instrs:
    raise SystemExit("FAIL: tan expected COS(16), SIN(15), RCP(2), MUL(2)")

cos_idx, cos_w = cos_instrs[0]
sin_idx, sin_w = sin_instrs[0]
rcp_idx, rcp_w = rcp_tan_instrs[0]
mul_idx, mul_w = mul_tan_instrs[0]

if not (cos_idx < rcp_idx and sin_idx < mul_idx and rcp_idx < mul_idx):
    raise SystemExit(f"FAIL: tan ordering constraint violated: expected cos({cos_idx}) < rcp({rcp_idx}) and {sin_idx, rcp_idx} < mul({mul_idx})")

cos_mask = (cos_w[3] >> 17) & 0xF
if cos_mask not in mask_to_lane:
    raise SystemExit(f"FAIL: tan COS writemask {cos_mask} is not a single lane mask")
cos_lane = mask_to_lane[cos_mask]
cos_dst = (cos_w[3] >> 7) & 0x3F

sr2_rcp_tan = (((rcp_w[2] >> 0) & 0x3F) << 11) | ((rcp_w[3] >> 21) & 0x7FF)
rcp_src_type = sr2_rcp_tan & 3
rcp_src_reg = (sr2_rcp_tan >> 2) & 0x1F
rcp_src_swz = (sr2_rcp_tan >> 14) & 3
if rcp_src_type != 1 or rcp_src_reg != cos_dst:
    raise SystemExit(f"FAIL: tan RCP does not consume COS dst temp {cos_dst} (src_type={rcp_src_type}, reg={rcp_src_reg})")
if rcp_src_swz != cos_lane:
    raise SystemExit(f"FAIL: tan RCP swizzle {rcp_src_swz} does not select COS written lane {cos_lane}")

rcp_mask = (rcp_w[3] >> 17) & 0xF
if rcp_mask not in mask_to_lane:
    raise SystemExit(f"FAIL: tan RCP writemask {rcp_mask} is not a single lane mask")
rcp_lane = mask_to_lane[rcp_mask]
rcp_dst = (rcp_w[3] >> 7) & 0x3F

sin_mask = (sin_w[3] >> 17) & 0xF
if sin_mask not in mask_to_lane:
    raise SystemExit(f"FAIL: tan SIN writemask {sin_mask} is not a single lane mask")
sin_lane = mask_to_lane[sin_mask]
sin_dst = (sin_w[3] >> 7) & 0x3F

# MUL consumes SIN and RCP
sr0_mul = (((mul_w[1] >> 0) & 0xFF) << 9) | ((mul_w[2] >> 23) & 0x1FF)
sr1_mul = (mul_w[2] >> 6) & 0x1FFFF
sr0_info = (sr0_mul & 3, (sr0_mul >> 2) & 0x1F, (sr0_mul >> 14) & 3)
sr1_info = (sr1_mul & 3, (sr1_mul >> 2) & 0x1F, (sr1_mul >> 14) & 3)

expected_sin = (1, sin_dst, sin_lane)
expected_rcp = (1, rcp_dst, rcp_lane)
if not ((sr0_info == expected_sin and sr1_info == expected_rcp) or
        (sr0_info == expected_rcp and sr1_info == expected_sin)):
    raise SystemExit(f"FAIL: tan MUL does not consume SIN {expected_sin} and RCP {expected_rcp}; got sr0={sr0_info}, sr1={sr1_info}")

print("All fixture ucode assertions & operand dependencies PASSED (including 7 full container twins and literal sqrt controls)")
PY

python3 "$root/tests/shader-compiler/vp_scalar_literal_check.py" "$compiler"

# Verify the 7 SDK shaders if SDK root is available
sdk_root="${PS3_SDK_ROOT:-/c/SDKs/Sony/SCE/PS3/475}"
if [[ -d "$sdk_root" ]]; then
    sdk_rows=(
        "sample_data/graphics/shaders/Tutorial/vs_procAnim.cg"
        "samples/tutorial/ParticleSimulator/05_spu_particles_with_shader/point_sprite_shader_vertex.cg"
        "samples/tutorial/ParticleSimulator/06_spu_particles_soa/point_sprite_shader_vertex.cg"
        "samples/tutorial/ParticleSimulator/07_spu_particles_spurs_task/point_sprite_shader_vertex.cg"
        "samples/tutorial/ParticleSimulator/08_spu_particles_gl_events/point_sprite_shader_vertex.cg"
        "samples/tutorial/ParticleSimulator/09_spu_particles_spurs_job/point_sprite_shader_vertex.cg"
        "samples/tutorial/ParticleSimulator/10_spu_particles_gcm/point_sprite_shader_vertex.cg"
    )

    count=0
    for i in "${!sdk_rows[@]}"; do
        rel="${sdk_rows[$i]}"
        src="$sdk_root/$rel"
        [[ -f "$src" ]] || fail "SDK row not found: $src"
        stem="$(basename "$rel" .cg)"
        dir="$(dirname "$src")"
        out_vpo="$work/${stem}_row${i}.vpo"
        rm -f "$out_vpo"
        rc=0
        (
            ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
            timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" -p sce_vp_rsx                 -I "$dir" -I "$sdk_root/samples/tutorial/DeferredShading/include"                 -I "$sdk_root/samples/tutorial/SpuGraphics/SpuRender/common/include"                 --emit-container "$out_vpo" "$src"
        ) >"$work/${stem}_row${i}.log" 2>&1 || rc=$?
        if [[ "$rc" != 0 || ! -s "$out_vpo" ]]; then
            tail -n 10 "$work/${stem}_row${i}.log" >&2
            fail "SDK row $rel did not compile (exit $rc)"
        fi
        # Parse and validate container structure
        python3 - "$out_vpo" "$rel" <<'VAL_PY'
import sys, struct, pathlib
path = pathlib.Path(sys.argv[1])
rel = sys.argv[2]
data = path.read_bytes()
if len(data) < 32:
    raise SystemExit(f"FAIL: {rel} container shorter than 32-byte header")
magic, rev, total, nparams, param_off, prog_off, ucode_sz, ucode_off = struct.unpack_from(">8I", data, 0)
if total != len(data):
    raise SystemExit(f"FAIL: {rel} declared total {total} != file size {len(data)}")
if ucode_sz == 0 or ucode_sz % 16 != 0:
    raise SystemExit(f"FAIL: {rel} invalid ucode size {ucode_sz}")
if ucode_off + ucode_sz > total:
    raise SystemExit(f"FAIL: {rel} ucode extent exceeds container size")
VAL_PY
        count=$((count + 1))
    done
    [[ "$count" -eq 7 ]] || fail "Expected 7 SDK rows, tested $count"
    printf 'All 7 SDK rows compiled and validated successfully (%d rows verified)
' "$count"
else
    printf 'SKIPPED SDK corpus: %s not found\n' "$sdk_root"
fi

printf 'PASS: vp-scalar-intrinsic-test
'
