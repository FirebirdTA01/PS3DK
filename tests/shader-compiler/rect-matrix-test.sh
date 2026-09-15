#!/usr/bin/env bash
# rect-matrix-test.sh -- non-square matrix types float3x4 / float4x3 / float2x4 /
# half3x4 (t_bc130064, t_69aeaa84, t_a5dbcca2).
#
# Reference contract (sce-cgc 475, measured 2026-09-15, .local/probe-rect): an
# RxC matrix is R rows of C-wide vectors.  mul(M[RxC], v[C]) -> v[R] as one
# DP(C) per row; mul(v[R], M[RxC]) -> v[C] as a MUL/MAD chain over the rows;
# mul(M[RxK], N[KxC]) -> M[RxC]; M[i] is a row of width C and M[i][j] a lane;
# transpose(3x4) is a 4x3; (float3x4)float4x4 keeps the first three rows.  A
# constructor takes exactly one row vector of width C per row, or R*C
# scalars: float4x3(f4,f4,f4) and float3x4(f3,f3,f3,f3) are refused C5204 even
# though the component totals agree.  The constructor and brace spellings are
# byte-identical to each other on the reference, and M[i] of either is
# byte-identical to the row expression, so those pairs are byte twins here.
# Container records: parent kCgFloat1x1 + (rows-1)*4 + (cols-1) (3x4 = 1060,
# 4x3 = 1063, 2x4 = 1056; half matrices carry the float codes in FP), one row
# record per ROW typed by the COLUMN count (1048 = float4, 1047 = float3);
# uniform arrays record every element, unreferenced ones with an empty
# resource, and VP rows take consecutive c-registers from c[256].
#
# Values: our FP ucode is executed numerically on an exact binary input and
# compared with an independent row/column formula; VP programs likewise with
# uniform rows supplied from the container's own records.  Byte identity to
# the reference is measured separately, never assumed here: our FP output for
# even square matrices differs from the reference in input copies and export
# folds, so a reference-byte row would test the wrong thing.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
shaders_dir="$repo_root/tools/rsx-cg-compiler/tests/shaders"

compiler="${1:-${RSX_CG_COMPILER:-$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler.exe}}"
if [[ ! -x "$compiler" ]]; then
    if [[ -x "$repo_root/build/rsx-cg-compiler.exe" ]]; then
        compiler="$repo_root/build/rsx-cg-compiler.exe"
    else
        echo "FAIL: rsx-cg-compiler binary not found at $compiler" >&2
        exit 1
    fi
fi

# A fresh scratch directory every run; accept outputs are removed before each
# launch so a compiler that writes nothing cannot pass on stale containers.
scratch_root="${TMPDIR:-$repo_root/.local/tmp}"
mkdir -p "$scratch_root"
work="$(mktemp -d "$scratch_root/rect-matrix-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

profile_of() {
    case "$1" in
        *_v) echo sce_vp_rsx ;;
        *) echo sce_fp_rsx ;;
    esac
}

ext_of() {
    case "$1" in
        *_v) echo vpo ;;
        *) echo fpo ;;
    esac
}

accept() {
    local name="$1" why="$2" rc=0 out
    out="$work/$name.$(ext_of "$name")"
    rm -f "$out"
    (
        "$compiler" -p "$(profile_of "$name")" --emit-container "$out" "$shaders_dir/$name.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    if [[ "$rc" != 0 ]]; then
        tail -n 8 "$work/$name.log" >&2
        fail "$name did not compile (exit $rc): $why"
    fi
    [[ -s "$out" ]] || fail "$name: compiler wrote no container ($why)"
    printf '  PASS  %-32s accepted: %s\n' "$name" "$why"
}

refuse() {
    local name="$1" pattern="$2" why="$3" rc=0 out
    out="$work/$name.$(ext_of "$name")"
    rm -f "$out"
    (
        "$compiler" -p "$(profile_of "$name")" --emit-container "$out" "$shaders_dir/$name.cg"
    ) >"$work/$name.log" 2>&1 || rc=$?
    [[ "$rc" != 0 ]] || fail "$name compiled; it must refuse ($why)"
    [[ "$rc" == 1 ]] || fail "$name exited $rc, not the refusal status 1 ($why)"
    [[ ! -e "$out" ]] || fail "$name refused but left an output file (even an empty one is a leak)"
    grep -qE "$pattern" "$work/$name.log" || { tail -n 4 "$work/$name.log" >&2; fail "$name refused for another reason than '$pattern' ($why)"; }
    printf '  PASS  %-32s refused: %s\n' "$name" "$why"
}

twin() {
    local a="$1" b="$2" why="$3"
    cmp -s "$work/$a.$(ext_of "$a")" "$work/$b.$(ext_of "$b")" || fail "$a and $b differ; the reference emits identical bytes ($why)"
    printf '  PASS  %-32s byte twin of %s: %s\n' "$a" "$b" "$why"
}

# 1. Uniform rectangular matrices in both orders, both profiles
accept fp_rect_uniform_m34_f       "mul(float3x4, float4) -> float3, one DP4 per row"
accept fp_rect_uniform_v3_m34_f    "mul(float3, float3x4) -> float4, MUL/MAD over the rows"
accept fp_rect_uniform_v4_m43_f    "mul(float4, float4x3) -> float3"
accept fp_rect_uniform_m43_v3_f    "mul(float4x3, float3) -> float4, one DP3 per row"
accept fp_rect_uniform_f2x4_h3x4_f "float2x4 and half3x4 uniforms"
accept fp_rect_row_access_f        "M[1] is a float4 row, M[2].z and M[0][1] are lanes"
accept vp_rect_bones_array_v       "uniform float3x4 bones[3], element 1 read"
accept vp_rect_bones_blend_v       "bones[0]*p.x + bones[1]*p.y blends a float3x4"
accept vp_rect_uniform_h3x4_v      "half3x4 uniform in a vertex program"

# 2. Local spellings whose VALUES the checker executes
accept fp_rect_ctor_m34_mul_f      "float3x4(three float4 rows) then mul(M, v)"
accept fp_rect_ctor_m43_mul_f      "float4x3(four float3 rows) then mul(P, v3)"
accept fp_rect_ctor_v4_m43_f       "mul(v4, float4x3)"
accept fp_rect_ctor_v3_m34_f       "mul(v3, float3x4)"
accept fp_rect_ctor_index_f        "row and lane indexing of a 3x4"
accept fp_rect_ctor_index_twin_f   "the same lanes spelled as swizzles"
accept fp_rect_brace_m43_f         "brace-initialised float4x3"
accept fp_rect_ctor_m43_twin_f     "constructor-initialised float4x3 (same rows)"
accept fp_rect_brace_row_f         "P[1] of a brace-initialised 4x3"
accept fp_rect_brace_row_twin_f    "the row expression itself"
accept fp_rect_transpose_f         "transpose(float3x4) is a float4x3"
accept fp_rect_narrow_cast_f       "(float3x4)float4x4 keeps the first three rows"
accept fp_rect_matmul_f            "mul(float3x4, float4x3) is a float3x3"
accept fp_rect_prefix_shuffle_cse_f "float4(t.yzw, 1) + t.yzwx: a vec3 and a vec4 shuffle with one mask stay distinct (reference accepts)"

# 2b. Mixed vector/scalar constructors pack ROW-MAJOR; a vector may not straddle
# a row (reference m1-m11).  The square globals here are a regression control:
# the first revision refused them (found by codex).  All four accept cells are
# byte-identical to the reference as well as to their twins.
accept fp_rect_ctor_mixed_g22_f       "static const float2x2(float2, s, s)"
accept fp_rect_ctor_mixed_g22_twin_f  "the literal row it folds to"
accept fp_rect_ctor_mixed_g33_f       "static const float3x3(float3, s, s, s, float3)"
accept fp_rect_ctor_mixed_g33_twin_f  "the literal row it folds to"
accept fp_rect_ctor_mixed_g34_f       "static const float3x4(float2, float2, float4, float4)"
accept fp_rect_ctor_mixed_g34_twin_f  "the same rows as float3x4(float3, s, float4, float4)"

# 3. Byte twins the reference also emits identically
twin fp_rect_ctor_mixed_g22_f fp_rect_ctor_mixed_g22_twin_f "a mixed 2x2 folds to its second row"
twin fp_rect_ctor_mixed_g33_f fp_rect_ctor_mixed_g33_twin_f "a mixed 3x3 folds to its second row"
twin fp_rect_ctor_mixed_g34_f fp_rect_ctor_mixed_g34_twin_f "two mixed packings of one 3x4"
twin fp_rect_ctor_index_f  fp_rect_ctor_index_twin_f "M[1].z, M[2][0], M[0].w are the row swizzles"
twin fp_rect_brace_m43_f   fp_rect_ctor_m43_twin_f   "brace and constructor spellings of a 4x3"
twin fp_rect_brace_row_f   fp_rect_brace_row_twin_f  "P[1] is the row expression"

# 4. Constructor shapes the reference refuses (C5204): a row vector per ROW
refuse fp_rect_ctor43_3xf4_refuse_f "C5204" "float4x3 from three float4 (12 components, wrong shape)"
refuse fp_rect_ctor34_4xf3_refuse_f "C5204" "float3x4 from four float3 (12 components, wrong shape)"
refuse fp_rect_ctor_straddle_g22_refuse_f "C5204" "float2x2(s, float2, s): the float2 straddles rows 0 and 1"
refuse fp_rect_ctor_straddle_g43_refuse_f "C5204" "float4x3(float2, float2, ...): the second float2 straddles rows 0 and 1"
refuse fp_rect_ctor_straddle_l22_refuse_f "C5204" "a local float2x2(s, float2, s) straddles the same way"

# 5. Values and container records
python3 "$script_dir/rect_matrix_check.py" "$work" || fail "rect_matrix_check reported a defect"

# 6. Self-check: a compiler that never writes must fail the accept rows even
# beside seeded scratch directories, and a wrong-status refusal must fail.
if [[ -z "${RECT_MATRIX_SELFCHECK:-}" ]]; then
    stub="$work/never-writes.sh"
    cat >"$stub" <<'STUB'
#!/usr/bin/env bash
case "$*" in *_refuse_*) echo "error C5204: stub" >&2; exit 1;; *) exit 0;; esac
STUB
    chmod +x "$stub"
    seeded_root="$(mktemp -d "$scratch_root/rect-matrix-seed.XXXXXX")"
    seeded="$(mktemp -d "$seeded_root/rect-matrix-test.XXXXXX")"
    cp "$work"/*.fpo "$work"/*.vpo "$seeded/"
    control_rc=0
    RECT_MATRIX_SELFCHECK=1 TMPDIR="$seeded_root" bash "$0" "$stub" >"$work/never-writes.log" 2>&1 || control_rc=$?
    rm -rf "$seeded_root"
    [[ $control_rc -eq 1 ]] || { cat "$work/never-writes.log" >&2; fail "self-check: the never-writes stub exited $control_rc, not the guard's own failure status 1"; }
    grep -q "compiler wrote no container" "$work/never-writes.log" || { cat "$work/never-writes.log" >&2; fail "self-check: the never-writes stub failed for another reason than the wrote-no-container assertion"; }
    printf '  PASS  self-check: a compiler that never writes fails the accept rows\n'

    leak="$work/leaks-empty.sh"
    cat >"$leak" <<LEAK
#!/usr/bin/env bash
out=""; args=("\$@"); while [ \$# -gt 0 ]; do [ "\$1" = --emit-container ] && out=\$2; shift; done
case "\$out" in *_refuse_*) : > "\$out"; echo "error C5204: stub" >&2; exit 1;; *) exec "$compiler" "\${args[@]}";; esac
LEAK
    chmod +x "$leak"
    control_rc=0
    RECT_MATRIX_SELFCHECK=1 bash "$0" "$leak" >"$work/leaks-empty.log" 2>&1 || control_rc=$?
    [[ $control_rc -eq 1 ]] || { cat "$work/leaks-empty.log" >&2; fail "self-check: the leaking-refusal stub exited $control_rc, not 1"; }
    grep -q "left an output file" "$work/leaks-empty.log" || { cat "$work/leaks-empty.log" >&2; fail "self-check: the leaking-refusal stub failed for another reason than the left-an-output-file assertion"; }
    printf '  PASS  self-check: a refusal that leaks an empty file fails the refusal row\n'
fi

printf 'PASS: rect-matrix-test\n'
