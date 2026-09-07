#!/usr/bin/env bash
# t_65e1b7fa: file-scope static const, const static, and static declarations.
#
# Asserts:
# 1. 'static const' and 'const static' variables are lowered as typed IR constants
#    rather than falling through to unregistered uniform loads ('ldunif of INFINITY').
# 2. Both const spellings enforce immutability: assignment to a static const or
#    const static identifier fails compilation with "expression is not assignable".
# 3. Typed integer scalar const folding:
#    - 'static const int A=5, B=2; float(A/B)' evaluates integer division to 2 (0x40000000),
#      NOT float division 2.5 (0x40200000).
#    - 'static const int K=7; float(K & 3)' folds bitwise AND to 3.0f (0x40400000).
# 4. Plain mutable static ('static float s = 2.0') with helper mutation is REFUSED
#    rather than silently compiled to wrong numbers (parent refuses unregistered 's').
# 5. Zero uniform slots: the container parameter table carries 0 uniform parameters
#    for static const and const static variables.
# 6. Ucode carries immediate constant values: 255.0f (0x437f0000), 2.0f (0x40000000).
# 7. Both fragment and vertex profiles are checked.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
python="${PYTHON:-python}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    for cand in "$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler" \
                "$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler.exe" \
                "$repo_root/build-win/rsx-cg-compiler.exe"; do
        if [[ -x "$cand" ]]; then compiler="$cand"; break; fi
    done
fi
[[ -n "$compiler" && -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-static-const-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# -----------------------------------------------------------------------------
# Row 1: fp_static_const_infinity_f.cg (positive compile + container check)
# -----------------------------------------------------------------------------
fpo="$work/fp_static_const.fpo"
ir_log="$work/fp_static_const_ir.log"

"$compiler" -p sce_fp_rsx --dump-ir "$shaders/fp_static_const_infinity_f.cg" >"$ir_log" 2>&1 || {
    tail -n 20 "$ir_log" >&2
    fail "fp_static_const_infinity_f failed to compile"
}

# Declared-type / no-ldunif evidence in IR:
grep -q "ldunif.*INFINITY" "$ir_log" && fail "IR contains ldunif for INFINITY; must be lowered as constant"
grep -q "ldunif.*EPSILON" "$ir_log" && fail "IR contains ldunif for EPSILON; must be lowered as constant"
grep -q "ldunif.*SCALE" "$ir_log" && fail "IR contains ldunif for SCALE; must be lowered as constant"

"$compiler" -p sce_fp_rsx --emit-container "$fpo" "$shaders/fp_static_const_infinity_f.cg" >"$work/fp_emit.log" 2>&1 || {
    tail -n 20 "$work/fp_emit.log" >&2
    fail "fp_static_const_infinity_f failed to emit container"
}

# Python container assertion: check parameter table and immediate constants
"$python" - "$fpo" <<'PY'
import struct
import sys

blob = open(sys.argv[1], "rb").read()
prof, rev, total, pcount, parr, prog, usize, ucode = struct.unpack_from(">8I", blob, 0)

# Check parameters: must contain only c and o, no INFINITY, EPSILON, or SCALE
for i in range(pcount):
    base = parr + i * 48
    name_off = struct.unpack_from(">I", blob, base + 16)[0]
    name = blob[name_off:blob.index(b"\0", name_off)].decode("ascii", "replace")
    if name in ("INFINITY", "EPSILON", "SCALE"):
        raise SystemExit(f"FAIL: container parameter table contains uniform '{name}', expected 0 uniform slots")

# Check ucode for immediate 255.0f (halfword-swapped 0000 437F)
# 255.0f = 0x437f0000 -> bytes: 00 00 43 7f
needle_inf = bytes((0x00, 0x00, 0x43, 0x7F))
if needle_inf not in blob[ucode:ucode+usize]:
    raise SystemExit("FAIL: ucode does not contain immediate 255.0f (0000 437F)")

# Check ucode for immediate 2.0f (halfword-swapped 0000 4000)
needle_two = bytes((0x00, 0x00, 0x40, 0x00))
if needle_two not in blob[ucode:ucode+usize]:
    raise SystemExit("FAIL: ucode does not contain immediate 2.0f (0000 4000)")
PY

# -----------------------------------------------------------------------------
# Row 2: Const immutability assertions (negative compile checks)
# -----------------------------------------------------------------------------
cat >"$work/neg_static_const.cg" <<'EOF'
static const float K = 1.0;
void main(float4 c : TEXCOORD0, out float4 o : COLOR) {
    K = 2.0;
    o = c * K;
}
EOF

neg_log="$work/neg_static_const.log"
if "$compiler" -p sce_fp_rsx "$work/neg_static_const.cg" >"$neg_log" 2>&1; then
    fail "reassignment to 'static const float K' compiled, expected 'expression is not assignable'"
fi
grep -q "expression is not assignable" "$neg_log" || {
    tail -n 10 "$neg_log" >&2
    fail "'static const' reassignment failed without expected diagnostic 'expression is not assignable'"
}

cat >"$work/neg_const_static.cg" <<'EOF'
const static float K = 1.0;
void main(float4 c : TEXCOORD0, out float4 o : COLOR) {
    K = 2.0;
    o = c * K;
}
EOF

neg_cs_log="$work/neg_const_static.log"
if "$compiler" -p sce_fp_rsx "$work/neg_const_static.cg" >"$neg_cs_log" 2>&1; then
    fail "reassignment to 'const static float K' compiled, expected 'expression is not assignable'"
fi
grep -q "expression is not assignable" "$neg_cs_log" || {
    tail -n 10 "$neg_cs_log" >&2
    fail "'const static' reassignment failed without expected diagnostic 'expression is not assignable'"
}

# -----------------------------------------------------------------------------
# Row 3: Typed integer scalar const folding (positive compile + evaluation check)
# -----------------------------------------------------------------------------
cat >"$work/pos_int_const.cg" <<'EOF'
static const int A = 5;
static const int B = 2;
static const int K = 7;
static const int K_LARGE = 16777217;
void main(float4 c : TEXCOORD0, out float4 o : COLOR) {
    float div_val = float(A / B);
    float bit_val = float(K & 3);
    float bit_large = float(K_LARGE & 1);
    o = c * div_val + bit_val + bit_large;
}
EOF

int_fpo="$work/int_const.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$int_fpo" "$work/pos_int_const.cg" >"$work/int.log" 2>&1 || {
    tail -n 20 "$work/int.log" >&2
    fail "typed int static const failed to compile"
}

"$python" - "$int_fpo" <<'PY'
import struct
import sys

blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# A / B must evaluate as integer division: 5 / 2 = 2 (2.0f -> 0x40000000 -> 00 00 40 00)
# and must NOT evaluate as float division 2.5 (2.5f -> 0x40200000 -> 00 00 40 20).
needle_two = bytes((0x00, 0x00, 0x40, 0x00))
needle_two_half = bytes((0x00, 0x00, 0x40, 0x20))
if needle_two not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain integer-folded 2.0f (0000 4000)")
if needle_two_half in u_bytes:
    raise SystemExit("FAIL: ucode contains float-divided 2.5f (0000 4020), expected integer fold 2")

# K & 3 must evaluate bitwise AND: 7 & 3 = 3 (3.0f -> 0x40400000 -> 00 00 40 40)
needle_three = bytes((0x00, 0x00, 0x40, 0x40))
if needle_three not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain bitwise-folded 3.0f (0000 4040)")

# K_LARGE & 1 must preserve integer precision without float32 truncation:
# 16777217 & 1 = 1 (1.0f -> 0x3f800000 -> 00 00 3f 80)
needle_one = bytes((0x00, 0x00, 0x3f, 0x80))
if needle_one not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain precision-preserved 1.0f (0000 3f80) for 16777217 & 1")
PY

# -----------------------------------------------------------------------------
# Row 3b: Signed integer division and remainder overflow robustness
# INT32_MIN / -1 and INT32_MIN % -1 must not crash with 0xC0000095
# -----------------------------------------------------------------------------
cat >"$work/pos_overflow_div.cg" <<'EOF'
static const int A = -2147483648;
static const int B = -1;
float4 main() : COLOR {
    float x = float(A / B);
    return float4(x, 0.0, 0.0, 1.0);
}
EOF
"$compiler" -p sce_fp_rsx "$work/pos_overflow_div.cg" >"$work/div_ovf.log" 2>&1 || {
    tail -n 20 "$work/div_ovf.log" >&2
    fail "signed division overflow INT32_MIN / -1 failed unexpectedly"
}

cat >"$work/pos_overflow_mod.cg" <<'EOF'
static const int A = -2147483648;
static const int B = -1;
float4 main() : COLOR {
    float x = float(A % B);
    return float4(x, 0.0, 0.0, 1.0);
}
EOF
"$compiler" -p sce_fp_rsx "$work/pos_overflow_mod.cg" >"$work/mod_ovf.log" 2>&1 || {
    tail -n 20 "$work/mod_ovf.log" >&2
    fail "signed remainder overflow INT32_MIN % -1 failed unexpectedly"
}

# Local INT_MIN / -1 must not crash the compiler host process (0xC0000095)
cat >"$work/pos_local_overflow_div.cg" <<'EOF'
float4 main() : COLOR {
    int a = -2147483648;
    int b = -1;
    return float(a / b);
}
EOF
"$compiler" -p sce_fp_rsx "$work/pos_local_overflow_div.cg" >"$work/local_div_ovf.log" 2>&1 || {
    tail -n 20 "$work/local_div_ovf.log" >&2
    fail "local division overflow INT32_MIN / -1 failed unexpectedly"
}

# -----------------------------------------------------------------------------
# Row 3c: Unsigned integer division, plain const integer conversions, and constructor casts
# -----------------------------------------------------------------------------
cat >"$work/pos_unsigned_div.cg" <<'EOF'
static const unsigned int A = 2147483648;
static const unsigned int B = 2;
const int K = 2147483647;
static const int BOOL_CONV = bool(2);
float4 main() : COLOR {
    float u_div = float(A / B);
    float k_val = float(K);
    float b_val = float(BOOL_CONV);
    return float4(u_div, k_val, b_val, 0.0);
}
EOF

unsigned_fpo="$work/unsigned_div.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$unsigned_fpo" "$work/pos_unsigned_div.cg" >"$work/unsigned.log" 2>&1 || {
    tail -n 20 "$work/unsigned.log" >&2
    fail "unsigned int division and plain const max failed to compile"
}

"$python" - "$unsigned_fpo" <<'PY'
import struct
import sys

blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# A / B must evaluate unsigned division: 2147483648 / 2 = 1073741824 (1073741824.0f -> 0x4e800000 -> 00 00 4e 80)
# and must NOT evaluate as signed division -1073741824 (-1073741824.0f -> 0xce800000 -> 00 00 ce 80).
needle_pos = bytes((0x00, 0x00, 0x4E, 0x80))
needle_neg = bytes((0x00, 0x00, 0xCE, 0x80))
if needle_pos not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain unsigned-folded +1073741824.0f (0000 4e80)")
if needle_neg in u_bytes:
    raise SystemExit("FAIL: ucode contains signed-divided -1073741824.0f (0000 ce80), expected unsigned +1073741824.0f")

# const int K = 2147483647 must preserve positive float conversion: +2147483648.0f (0x4f000000 -> 00 00 4f 00)
# and must NOT overflow/round to negative -2147483648.0f (0xcf000000 -> 00 00 cf 00).
needle_k_pos = bytes((0x00, 0x00, 0x4F, 0x00))
needle_k_neg = bytes((0x00, 0x00, 0xCF, 0x00))
if needle_k_pos not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain plain const float(K) +2147483648.0f (0000 4f00)")
if needle_k_neg in u_bytes:
    raise SystemExit("FAIL: ucode contains negative float(K) -2147483648.0f (0000 cf00)")

# static const int BOOL_CONV = bool(2) must apply constructor conversion: bool(2) = 1 (1.0f -> 0x3f800000 -> 00 00 3f 80)
# and must NOT treat constructor as identity 2 (2.0f -> 0x40000000 -> 00 00 40 00).
needle_b_pos = bytes((0x00, 0x00, 0x3F, 0x80))
needle_b_neg = bytes((0x00, 0x00, 0x40, 0x00))
if needle_b_pos not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain constructor-converted bool(2) -> 1.0f (0000 3f80)")
if needle_b_neg in u_bytes:
    raise SystemExit("FAIL: ucode contains unconverted bool(2) -> 2.0f (0000 4000)")
PY

# -----------------------------------------------------------------------------
# Row 3d: Typed constant conversion edge cases
# - unsigned int A=1; float(-A) evaluates to +4294967296.0f (NOT signed -1.0f)
# - short(65537) truncates to 1.0f (NOT unconverted 65537.0f)
# - const int K = float(2147483647); float(K) preserves positive -> +2147483648.0f
# - static const int K = float(16777217); float(K & 1) preserves precision -> 1.0f
# - bool(0.5) evaluates to 1.0f (NOT 0.0f from premature float truncation)
# - !0.5 evaluates to 0.0f (NOT 1.0f from premature float truncation)
# - int2(1.5, 2.5) evaluates component-wise to 1.0f, 2.0f (NOT 1.5f, 2.5f)
# - unsupported file-scope const expression is refused with clean named diagnostic
# -----------------------------------------------------------------------------
cat >"$work/pos_typed_eval.cg" <<'EOF'
static const int S_TRUNC = short(65537);
static const unsigned int U_ONE = 1;
const int K_MAX_C = float(2147483647);
static const int K_PREC_C = float(16777217);

float4 main() : COLOR {
    float u_neg = float(-U_ONE);
    float k_prec = float(K_PREC_C & 1);
    float k_max = float(K_MAX_C);
    float s_val = float(S_TRUNC);
    return float4(u_neg, k_prec, k_max, s_val);
}
EOF

typed_fpo="$work/typed_eval.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$typed_fpo" "$work/pos_typed_eval.cg" >"$work/typed.log" 2>&1 || {
    tail -n 20 "$work/typed.log" >&2
    fail "typed constant conversion edge cases failed to compile"
}

"$python" - "$typed_fpo" <<'PY'
import struct
import sys

blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# unsigned negation -1u -> 4294967295 -> float 4294967296.0f (0x4f800000 -> 00 00 4f 80)
# must NOT evaluate as signed -1.0f (0xbf800000 -> 00 00 bf 80)
needle_uneg_pos = bytes((0x00, 0x00, 0x4f, 0x80))
needle_uneg_neg = bytes((0x00, 0x00, 0xbf, 0x80))
if needle_uneg_pos not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain unsigned negation +4294967296.0f (0000 4f80)")
if needle_uneg_neg in u_bytes:
    raise SystemExit("FAIL: ucode contains signed negation -1.0f (0000 bf80)")

# K_PREC_C & 1: 16777217 & 1 = 1.0f (0x3f800000 -> 00 00 3f 80)
# short(65537) = 1.0f (0x3f800000 -> 00 00 3f 80)
needle_one = bytes((0x00, 0x00, 0x3f, 0x80))
if needle_one not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain 1.0f (0000 3f80)")

# short(65537) must NOT retain 65537.0f (0x47800080 -> 00 80 47 80)
needle_65537 = bytes((0x00, 0x80, 0x47, 0x80))
if needle_65537 in u_bytes:
    raise SystemExit("FAIL: ucode contains unconverted short(65537) -> 65537.0f (0080 4780)")

# const int K = float(2147483647) must evaluate to +2147483648.0f (0x4f000000 -> 00 00 4f 00)
# and must NOT evaluate to -2147483648.0f (0xcf000000 -> 00 00 cf 00)
needle_k_pos = bytes((0x00, 0x00, 0x4f, 0x00))
needle_k_neg = bytes((0x00, 0x00, 0xcf, 0x00))
if needle_k_pos not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain +2147483648.0f (0000 4f00)")
if needle_k_neg in u_bytes:
    raise SystemExit("FAIL: ucode contains negative -2147483648.0f (0000 cf00)")
PY

# Check bool(0.5) -> 1.0f
cat >"$work/pos_bool_float.cg" <<'EOF'
static const int B_HALF = bool(0.5);
float4 main() : COLOR {
    return float4(float(B_HALF), 0.0, 0.0, 0.0);
}
EOF
b_fpo="$work/bool_float.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$b_fpo" "$work/pos_bool_float.cg" >"$work/bool_flt.log" 2>&1 || {
    tail -n 20 "$work/bool_flt.log" >&2
    fail "bool(0.5) failed to compile"
}
"$python" - "$b_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]
if bytes((0x00, 0x00, 0x3f, 0x80)) not in u_bytes:
    raise SystemExit("FAIL: bool(0.5) ucode does not contain 1.0f (0000 3f80)")
PY

# Check !0.5 -> 0.0f
cat >"$work/pos_not_float.cg" <<'EOF'
static const int NOT_HALF = !0.5;
float4 main() : COLOR {
    return float4(float(NOT_HALF), 0.0, 0.0, 0.0);
}
EOF
not_fpo="$work/not_float.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$not_fpo" "$work/pos_not_float.cg" >"$work/not_flt.log" 2>&1 || {
    tail -n 20 "$work/not_flt.log" >&2
    fail "!0.5 failed to compile"
}
"$python" - "$not_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]
if bytes((0x00, 0x00, 0x3f, 0x80)) in u_bytes:
    raise SystemExit("FAIL: !0.5 ucode unexpectedly contains 1.0f (0000 3f80), expected 0.0f")
PY

# Check int2(1.5, 2.5) -> 1.0f, 2.0f (NOT 1.5f, 2.5f)
cat >"$work/pos_int2_float.cg" <<'EOF'
static const float2 V_INT2 = int2(1.5, 2.5);
float4 main() : COLOR {
    return float4(V_INT2.x, V_INT2.y, 0.0, 0.0);
}
EOF
int2_fpo="$work/int2_float.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$int2_fpo" "$work/pos_int2_float.cg" >"$work/int2_flt.log" 2>&1 || {
    tail -n 20 "$work/int2_flt.log" >&2
    fail "int2(1.5, 2.5) failed to compile"
}
"$python" - "$int2_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]
if bytes((0x00, 0x00, 0x3f, 0x80)) not in u_bytes:
    raise SystemExit("FAIL: int2(1.5, 2.5).x does not contain 1.0f (0000 3f80)")
if bytes((0x00, 0x00, 0x40, 0x00)) not in u_bytes:
    raise SystemExit("FAIL: int2(1.5, 2.5).y does not contain 2.0f (0000 4000)")
if bytes((0x00, 0x00, 0x3f, 0xc0)) in u_bytes:
    raise SystemExit("FAIL: int2(1.5, 2.5) ucode unexpectedly contains unconverted 1.5f (0000 3fc0)")
if bytes((0x00, 0x00, 0x40, 0x20)) in u_bytes:
    raise SystemExit("FAIL: int2(1.5, 2.5) ucode unexpectedly contains unconverted 2.5f (0000 4020)")
PY

# Check unsupported file-scope const initializer is refused by name (negative compile check)
cat >"$work/neg_unsupported_const.cg" <<'EOF'
static const int K = 1 + 2;
float4 main() : COLOR {
    return float4(float(K), 0.0, 0.0, 0.0);
}
EOF
unsupp_log="$work/neg_unsupported_const.log"
if "$compiler" -p sce_fp_rsx "$work/neg_unsupported_const.cg" >"$unsupp_log" 2>&1; then
    fail "unsupported file-scope const expression compiled; must be refused"
fi
grep -q "has an initialiser this compiler cannot evaluate; refusing rather than compiling it as zero" "$unsupp_log" || {
    tail -n 10 "$unsupp_log" >&2
    fail "unsupported file-scope const failed without expected refusal diagnostic"
}

# -----------------------------------------------------------------------------
# Row 3e: Target hardware semantics for out-of-range, half, and fixed conversions
# - int K = float(4294967296.0) folds to INT32_MIN (-2147483648.0f: 00 00 cf 00)
# - int K = 1e30 folds to INT32_MIN (-2147483648.0f: 00 00 cf 00)
# - static const half H = 1.00048828125; float(H) - 1.0 folds to 0.0009765625f (00 00 3a 80)
#   in 3 instructions (NOT 11 instructions from un-folded SSA %htof)
# - fixed F = 3.0 clamps to 1.9990234375f (e0 00 3f ff)
# - fixed F = -3.0 clamps to -2.0f (00 00 c0 00)
# -----------------------------------------------------------------------------
cat >"$work/pos_hw_semantics.cg" <<'EOF'
static const int K_2P32 = float(4294967296.0);
static const int K_1E30 = 1e30;
static const fixed F_POS = 3.0;
static const fixed F_NEG = -3.0;

float4 main() : COLOR {
    return float4(float(K_2P32), float(K_1E30), float(F_POS), float(F_NEG));
}
EOF

hw_fpo="$work/hw_semantics.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$hw_fpo" "$work/pos_hw_semantics.cg" >"$work/hw_sem.log" 2>&1 || {
    tail -n 20 "$work/hw_sem.log" >&2
    fail "hardware semantics conversions failed to compile"
}

"$python" - "$hw_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# INT32_MIN = -2147483648.0f (0xcf000000 -> 00 00 cf 00)
needle_int_min = bytes((0x00, 0x00, 0xcf, 0x00))
if needle_int_min not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain INT32_MIN (-2147483648.0f: 00 00 cf 00)")

# fixed 3.0 -> 1.9990234375f (0x3fffe000 -> e0 00 3f ff)
needle_fixed_pos = bytes((0xe0, 0x00, 0x3f, 0xff))
if needle_fixed_pos not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain clamped fixed 1.9990234375f (e0 00 3f ff)")

# fixed -3.0 -> -2.0f (0xc0000000 -> 00 00 c0 00)
needle_fixed_neg = bytes((0x00, 0x00, 0xc0, 0x00))
if needle_fixed_neg not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain clamped fixed -2.0f (00 00 c0 00)")
PY

cat >"$work/pos_half_round.cg" <<'EOF'
static const half H = 1.00048828125;
float4 main() : COLOR {
    return float4(float(H) - 1.0, 0.0, 0.0, 1.0);
}
EOF

half_fpo="$work/half_round.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$half_fpo" "$work/pos_half_round.cg" >"$work/half_round.log" 2>&1 || {
    tail -n 20 "$work/half_round.log" >&2
    fail "half rounding and folding failed to compile"
}

"$python" - "$half_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# half 1.00048828125 rounds to 1.0009765625 (tie round up); float(H) - 1.0 = 0.0009765625f (0x3a800000 -> 00 00 3a 80)
needle_half_diff = bytes((0x00, 0x00, 0x3a, 0x80))
if needle_half_diff not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain folded half diff 0.0009765625f (00 00 3a 80)")

inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count} (usize={usize})")
PY

cat >"$work/pos_half_neg_tie.cg" <<'EOF'
static const half H = -1.00048828125;
float4 main() : COLOR {
    return float4(float(H), 0.0, 0.0, 1.0);
}
EOF

half_neg_fpo="$work/half_neg_tie.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$half_neg_fpo" "$work/pos_half_neg_tie.cg" >"$work/half_neg.log" 2>&1 || {
    tail -n 20 "$work/half_neg.log" >&2
    fail "half negative tie rounding failed to compile"
}

"$python" - "$half_neg_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# half -1.00048828125: tie rounds toward +infinity to -1.0f (0xbf800000 -> 00 00 bf 80)
# and must NOT round away from zero to -1.0009765625f (0xbf802000 -> 20 00 bf 80)
needle_neg_tie_ok = bytes((0x00, 0x00, 0xbf, 0x80))
needle_neg_tie_bad = bytes((0x20, 0x00, 0xbf, 0x80))
if needle_neg_tie_ok not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain -1.0f (00 00 bf 80) for negative half tie -1.00048828125")
if needle_neg_tie_bad in u_bytes:
    raise SystemExit("FAIL: ucode contains -1.0009765625f (20 00 bf 80), expected round-toward-+infinity -1.0f")

inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count} (usize={usize})")
PY

cat >"$work/pos_fixed_neg_tie.cg" <<'EOF'
static const fixed K = -1.00048828125;
static const fixed K_ZERO = -0.00048828125;
float4 main() : COLOR {
    return float4(float(K), float(K_ZERO), 0.0, 1.0);
}
EOF

fixed_neg_fpo="$work/fixed_neg_tie.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$fixed_neg_fpo" "$work/pos_fixed_neg_tie.cg" >"$work/fixed_neg.log" 2>&1 || {
    tail -n 20 "$work/fixed_neg.log" >&2
    fail "fixed negative tie rounding failed to compile"
}

"$python" - "$fixed_neg_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# fixed -1.00048828125: tie rounds toward +infinity to -1.0f (0xbf800000 -> 00 00 bf 80)
# and must NOT round away from zero to -1.0009765625f (0xbf802000 -> 20 00 bf 80)
needle_fixed_neg_ok = bytes((0x00, 0x00, 0xbf, 0x80))
needle_fixed_neg_bad = bytes((0x20, 0x00, 0xbf, 0x80))
if needle_fixed_neg_ok not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain -1.0f (00 00 bf 80) for negative fixed tie -1.00048828125")
if needle_fixed_neg_bad in u_bytes:
    raise SystemExit("FAIL: ucode contains -1.0009765625f (20 00 bf 80), expected round-toward-+infinity -1.0f")

# fixed -0.00048828125: tie against zero rounds toward +infinity to 0.0f
# and must NOT round away from zero to -0.0009765625f (0xba800000 -> 00 00 ba 80)
needle_fixed_zero_bad = bytes((0x00, 0x00, 0xba, 0x80))
if needle_fixed_zero_bad in u_bytes:
    raise SystemExit("FAIL: ucode contains -0.0009765625f (00 00 ba 80) for tie against zero, expected 0.0f")

inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count} (usize={usize})")
PY

cat >"$work/pos_int2_div.cg" <<'EOF'
static const int2 K = int2(5, 7);
float4 main() : COLOR {
    return float4(float(K.x / 2), 2.0, 3.0, 4.0);
}
EOF

int2_div_fpo="$work/int2_div.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$int2_div_fpo" "$work/pos_int2_div.cg" >"$work/int2_div.log" 2>&1 || {
    tail -n 20 "$work/int2_div.log" >&2
    fail "int2 swizzle division failed to compile"
}

"$python" - "$int2_div_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# K.x / 2 must evaluate as integer division: 5 / 2 = 2 (2.0f -> 0x40000000 -> 00 00 40 00)
# and must NOT evaluate as float division 2.5 (2.5f -> 0x40200000 -> 00 00 40 20).
needle_two = bytes((0x00, 0x00, 0x40, 0x00))
needle_two_half = bytes((0x00, 0x00, 0x40, 0x20))
if needle_two not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain integer-folded 2.0f (00 00 40 00) for K.x / 2")
if needle_two_half in u_bytes:
    raise SystemExit("FAIL: ucode contains float-divided 2.5f (00 00 40 20), expected integer fold 2")

inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count} (usize={usize})")
PY

cat >"$work/pos_int2_large.cg" <<'EOF'
static const int2 K = int2(16777217, 16777219);
float4 main() : COLOR {
    return float4(float(K.x & 1), float(K.y & 2), 3.0, 4.0);
}
EOF

int2_large_fpo="$work/int2_large.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$int2_large_fpo" "$work/pos_int2_large.cg" >"$work/int2_large.log" 2>&1 || {
    tail -n 20 "$work/int2_large.log" >&2
    fail "int2 large integer precision failed to compile"
}

"$python" - "$int2_large_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# 16777217 & 1 = 1 (1.0f -> 0x3f800000 -> 00 00 3f 80)
# and must NOT evaluate to 0.0f (which happens if 16777217 is rounded to float 16777216)
needle_one = bytes((0x00, 0x00, 0x3f, 0x80))
needle_two = bytes((0x00, 0x00, 0x40, 0x00))
if needle_one not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain 1.0f (00 00 3f 80) for (16777217 & 1)")
if needle_two not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain 2.0f (00 00 40 00) for (16777219 & 2)")

inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count} (usize={usize})")
PY

# -----------------------------------------------------------------------------
# Row 3i: Local vector constructor boolean truthiness (Codex review witness)
# bool2(float2(0.5, 2.0)).x must evaluate to true (0.5 != 0)
# -----------------------------------------------------------------------------
cat >"$work/pos_local_bool2.cg" <<'EOF'
float4 main() : COLOR {
    bool2 b = bool2(float2(0.5, 2.0));
    return float4(float(b.x), float(b.y), 3.0, 4.0);
}
EOF

local_bool2_fpo="$work/local_bool2.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$local_bool2_fpo" "$work/pos_local_bool2.cg" >"$work/local_bool2.log" 2>&1 || {
    tail -n 20 "$work/local_bool2.log" >&2
    fail "local bool2 constructor failed to compile"
}

"$python" - "$local_bool2_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# Both b.x and b.y are truthy -> float(b.x)=1.0f (00 00 3f 80), float(b.y)=1.0f (00 00 3f 80)
needle_one = bytes((0x00, 0x00, 0x3f, 0x80))
if needle_one not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain 1.0f (00 00 3f 80) for truthy bool components")
inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count}")
PY

# -----------------------------------------------------------------------------
# Row 3j: Local integer vector constructor out-of-range float clamp (INT32_MIN)
# int2(float2(1e30, 4294967296.0)) must clamp to -2147483648 (0xcf000000)
# -----------------------------------------------------------------------------
cat >"$work/pos_local_int2_oob.cg" <<'EOF'
float4 main() : COLOR {
    int2 k = int2(float2(1e30, 4294967296.0));
    return float4(float(k.x), float(k.y), 3.0, 4.0);
}
EOF

local_int2_oob_fpo="$work/local_int2_oob.fpo"
"$compiler" -p sce_fp_rsx --emit-container "$local_int2_oob_fpo" "$work/pos_local_int2_oob.cg" >"$work/local_int2_oob.log" 2>&1 || {
    tail -n 20 "$work/local_int2_oob.log" >&2
    fail "local int2 out-of-range constructor failed to compile"
}

"$python" - "$local_int2_oob_fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], "rb").read()
_, _, _, _, _, _, usize, ucode = struct.unpack_from(">8I", blob, 0)
u_bytes = blob[ucode:ucode+usize]

# -2147483648.0f as IEEE 754 is 0xcf000000 -> 00 00 cf 00 in big-endian
needle_min = bytes((0x00, 0x00, 0xcf, 0x00))
if needle_min not in u_bytes:
    raise SystemExit("FAIL: ucode does not contain -2147483648.0f (00 00 cf 00) for INT32_MIN clamp")
inst_count = usize // 16
if inst_count != 3:
    raise SystemExit(f"FAIL: expected 3 instructions after folding, got {inst_count}")
PY

# -----------------------------------------------------------------------------
# Row 4: Bare mutable static exclusion / refusal (negative compile check)
# -----------------------------------------------------------------------------
cat >"$work/neg_mutable_static.cg" <<'EOF'
static float s = 2.0;
float inc() {
    s = s + 1.0;
    return s;
}
float4 main() : COLOR {
    float a = inc();
    return float4(a, s, 0.0, 1.0);
}
EOF

mut_log="$work/neg_mutable_static.log"
if "$compiler" -p sce_fp_rsx "$work/neg_mutable_static.cg" >"$mut_log" 2>&1; then
    fail "bare mutable static helper mutation unexpectedly compiled; must be refused"
fi
grep -q "ldunif of 's' has no registered uniform source" "$mut_log" || {
    tail -n 10 "$mut_log" >&2
    fail "bare mutable static failed without expected diagnostic 'ldunif of s'"
}

# -----------------------------------------------------------------------------
# Row 5: Vertex profile (vp_static_const_v.cg)
# -----------------------------------------------------------------------------
vp_fpo="$work/vp_static_const.vpo"
"$compiler" -p sce_vp_rsx --emit-container "$vp_fpo" "$shaders/vp_static_const_v.cg" >"$work/vp.log" 2>&1 || {
    tail -n 20 "$work/vp.log" >&2
    fail "vp_static_const_v failed to compile"
}
[[ -s "$vp_fpo" ]] || fail "vp_static_const_v produced no container"

printf 'static-const-qualifier-test: ok\n'