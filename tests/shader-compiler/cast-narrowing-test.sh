#!/usr/bin/env bash
# C-STYLE NARROWING CASTS ON VECTORS AND MATRICES (t_d03921c3).
#
# C-style casts narrowing vector or matrix dimensions:
#   - (float3)position lowers to position.xyz (VecShuffle leading components)
#   - (float3x3)model lowers to upper-left submatrix extraction
#     (VecExtract row 0..2 + VecShuffle leading components + MatConstruct)
#   - Single-argument narrowing constructors float3(v4) and float3x3(m4)
#     behave identically to the cast spelling.
#   - Widening casts and single-argument widening constructors are REFUSED
#     by the reference compiler with "error C1033: cast not allowed".
#
# The assertion is byte-identity against swizzle and row-constructor twins,
# near-miss wrong-value controls asserting difference against .xyw, and
# rejection of widening forms with error C1033.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-cast-narrowing.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

emit_vp() {   # <stem>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}" 2>/dev/null || true
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx --emit-container "$work/$stem.vpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    [[ "$rc" -ne 124 ]] || fail "$stem timed out"
    if [[ "$rc" -ne 0 ]]; then
        cat "$work/$stem.err" >&2
        fail "$stem did not compile with exit code $rc"
    fi
    [[ -s "$work/$stem.vpo" ]] || fail "$stem wrote no container"
}

emit_fp() {   # <stem>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}" 2>/dev/null || true
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    [[ "$rc" -ne 124 ]] || fail "$stem timed out"
    if [[ "$rc" -ne 0 ]]; then
        cat "$work/$stem.err" >&2
        fail "$stem did not compile with exit code $rc"
    fi
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
}

# 1. Compile vertex fixtures
for stem in vp_cast_narrow_vec3_v vp_cast_narrow_vec3_swz_v \
            vp_cast_narrow_vec3_ctor_v vp_cast_narrow_vec3_ctrl_v \
            vp_cast_narrow_mat3_v vp_cast_narrow_mat3_ctor_v \
            vp_cast_narrow_mat3_single_ctor_v vp_cast_narrow_mat3_ctrl_v; do
    emit_vp "$stem"
done

# 2. Compile fragment fixtures
for stem in fp_cast_narrow_vec3_f fp_cast_narrow_vec3_swz_f \
            fp_cast_narrow_vec3_ctrl_f; do
    emit_fp "$stem"
done

# 3. Byte-identity twin assertions
twin() {   # <stem1> <stem2> <ext> <what>
    local f1="$work/$1.$3" f2="$work/$2.$3"
    if ! cmp -s "$f1" "$f2"; then
        fail "$4: containers differ ($1 vs $2)"
    fi
}

twin vp_cast_narrow_vec3_v vp_cast_narrow_vec3_swz_v vpo \
    "vector cast (float3)v4 matches swizzle v4.xyz"
twin vp_cast_narrow_vec3_v vp_cast_narrow_vec3_ctor_v vpo \
    "vector cast (float3)v4 matches single-arg constructor float3(v4)"
twin vp_cast_narrow_mat3_v vp_cast_narrow_mat3_ctor_v vpo \
    "matrix cast (float3x3)m4 matches row constructor"
twin vp_cast_narrow_mat3_v vp_cast_narrow_mat3_single_ctor_v vpo \
    "matrix cast (float3x3)m4 matches single-arg constructor float3x3(m4)"
twin fp_cast_narrow_vec3_f fp_cast_narrow_vec3_swz_f fpo \
    "fragment vector cast (float3)t matches swizzle t.xyz"

# 4. Near-miss wrong-value controls (must NOT be identical)
control() {   # <stem1> <control_stem> <ext> <what>
    local f1="$work/$1.$3" f2="$work/$2.$3"
    if cmp -s "$f1" "$f2"; then
        fail "$4: control unexpectedly identical to target ($1 vs $2)"
    fi
}

control vp_cast_narrow_vec3_v vp_cast_narrow_vec3_ctrl_v vpo \
    "vector narrowing vs .xyw near-miss control"
control vp_cast_narrow_mat3_v vp_cast_narrow_mat3_ctrl_v vpo \
    "matrix narrowing vs .xyw near-miss control"
control fp_cast_narrow_vec3_f fp_cast_narrow_vec3_ctrl_f fpo \
    "fragment vector narrowing vs .xyw near-miss control"

# 5. Widening rejection assertions (must fail with error C1033)
refuse_widen() {   # <stem> <what>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}" 2>/dev/null || true
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    refusal_status "$rc" "$2"
    if [[ "$rc" -ne 1 ]]; then
        fail "$2: widening unexpectedly compiled with exit code $rc, expected 1"
    fi
    local combined
    combined="$(cat "$work/$stem.log" "$work/$stem.err")"
    if ! grep -Fq "error C1033: cast not allowed" <<<"$combined"; then
        fail "$2: failed but diagnostic did not contain 'error C1033: cast not allowed': $combined"
    fi
}

refuse_widen vp_cast_widen_vec4_v "vector cast widening (float4)float3"
refuse_widen vp_cast_widen_mat4_v "matrix cast widening (float4x4)float3x3"
refuse_widen vp_ctor_widen_vec4_v "vector constructor widening float4(float3)"
refuse_widen vp_ctor_widen_mat4_v "matrix constructor widening float4x4(float3x3)"

# 6. Constant conversion probes (fixed/half/char/short base types)
PYTHON_BIN="${PYTHON:-python3}"
checker="$work/check_vpo_const.py"

cat > "$checker" << 'PY'
import math, struct, sys

if len(sys.argv) < 5:
    sys.exit('FAIL: usage: check_vpo_const.py <vpo_path> <expected_f0> <expected_f1> <stem>')

vpo_path, exp0_str, exp1_str, stem = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
try:
    exp0 = float(exp0_str)
    exp1 = float(exp1_str)
except ValueError:
    sys.exit(f'FAIL: invalid numeric expected values ({exp0_str}, {exp1_str}) in {stem}')

blob = open(vpo_path, 'rb').read()
u32 = lambda o: struct.unpack_from('>I', blob, o)[0]
f32 = lambda o: struct.unpack_from('>f', blob, o)[0]

if len(blob) < 40:
    sys.exit(f'FAIL: container too short ({len(blob)} bytes) in {stem}')

pcount, parr = u32(12), u32(16)
default = None
for i in range(pcount):
    base = parr + i * 48
    d = u32(base + 20)
    if d:
        default = d
        break

if default is None:
    sys.exit(f'FAIL: no parameter default / literal block found in container: {stem}')

f0, f1 = f32(default), f32(default + 4)
if not math.isfinite(f0) or not math.isfinite(f1):
    sys.exit(f'FAIL: non-finite literal value in container: ({f0}, {f1}) for {stem}')

if abs(f0 - exp0) > 1e-4 or abs(f1 - exp1) > 1e-4:
    sys.exit(f'FAIL: mismatch in {stem}: got ({f0}, {f1}), expected ({exp0}, {exp1})')
PY
checker_win="$checker"
command -v cygpath >/dev/null 2>&1 && checker_win="$(cygpath -w "$checker")"

check_const() {  # <stem> <expected_f0> <expected_f1> <what>
    local stem="$1" exp0="$2" exp1="$3" what="$4"
    emit_vp "$stem"
    local vpo_win="$work/$stem.vpo"
    command -v cygpath >/dev/null 2>&1 && vpo_win="$(cygpath -w "$vpo_win")"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$vpo_win" "$exp0" "$exp1" "$stem" 2>&1)" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        fail "$what: constant check failed with rc=$rc: $out"
    fi
}

# 6a. Narrowing constant conversion probes
check_const vp_cast_narrow_fixed2_v 1.9990234375 -2.0 "fixed2 constant narrowing clamps [-2.0, 1.999]"
check_const vp_cast_narrow_half2_v 70016.0 -70016.0 "half2 constant narrowing rounds to fp16"
check_const vp_cast_narrow_char2_v 44.0 126.0 "char2 constant narrowing wraps int8"
check_const vp_cast_narrow_short2_v 1.0 32767.0 "short2 constant narrowing wraps int16"

# 6b. Half precision boundaries
check_const vp_cast_narrow_half2_bound_65504_v 65504.0 -65504.0 "half2 at max normal IEEE fp16 (65504)"
check_const vp_cast_narrow_half2_bound_65536_v 65536.0 -65536.0 "half2 at 2^16 (65536)"
check_const vp_cast_narrow_half2_bound_131008_v 131008.0 -131008.0 "half2 at max NV40 finite half (131008)"

# 6c. Non-narrowing half conversion controls (assert roundToHalf preserves non-narrowing)
check_const vp_ctor_half2_70000_v 70016.0 -70016.0 "non-narrowing half2 constructor twin at 70000"
check_const vp_cast_half2_float2_v 70016.0 -70016.0 "non-narrowing (half2)float2 cast at 70000"

# 6d. Negative controls: run SAME checker on wrong expected values and mutated NaN container
# Control 1: Wrong expected value must exit 1 with named mismatch diagnostic
check_wrong_val() {
    local stem="vp_cast_narrow_half2_v"
    local vpo_win="$work/$stem.vpo"
    command -v cygpath >/dev/null 2>&1 && vpo_win="$(cygpath -w "$vpo_win")"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker" "$vpo_win" "99999.0" "-70016.0" "$stem" 2>&1)" || rc=$?
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: wrong expected value exited $rc (expected 1)"
    fi
    if ! grep -q "FAIL: mismatch in $stem: got (70016.0, -70016.0), expected (99999.0, -70016.0)" <<<"$out"; then
        fail "negative control failed: wrong expected value did not produce named mismatch diagnostic: $out"
    fi
}
check_wrong_val

# Control 2: Mutated container with NaN in literal block must exit 1 with named non-finite diagnostic
check_nan_reject() {
    local stem="vp_cast_narrow_half2_v"
    local vpo_orig="$work/$stem.vpo"
    local vpo_nan="$work/${stem}_nan.vpo"
    command -v cygpath >/dev/null 2>&1 && vpo_orig="$(cygpath -w "$vpo_orig")"
    command -v cygpath >/dev/null 2>&1 && vpo_nan="$(cygpath -w "$vpo_nan")"

    # Mutate literal block to float('nan') in the container copy
    "$PYTHON_BIN" -c "
import struct
blob = bytearray(open(r'$vpo_orig', 'rb').read())
pcount, parr = struct.unpack_from('>II', blob, 12)
for i in range(pcount):
    d = struct.unpack_from('>I', blob, parr + i * 48 + 20)[0]
    if d:
        struct.pack_into('>f', blob, d, float('nan'))
        break
open(r'$vpo_nan', 'wb').write(blob)
"
    local out rc=0
    out="$("$PYTHON_BIN" "$checker_win" "$vpo_nan" "70016.0" "-70016.0" "$stem" 2>&1)" || rc=$?
    if [[ "$rc" -ne 1 ]]; then
        fail "negative control failed: NaN-mutated container exited $rc (expected 1)"
    fi
    if ! grep -q "FAIL: non-finite literal value in container" <<<"$out"; then
        fail "negative control failed: NaN container did not produce named non-finite diagnostic: $out"
    fi
}
check_nan_reject

printf 'cast-narrowing: PASS\n'



