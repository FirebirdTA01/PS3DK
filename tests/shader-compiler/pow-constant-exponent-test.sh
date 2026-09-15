#!/usr/bin/env bash
# pow-constant-exponent-test.sh -- pow() with a constant exponent follows the
# reference's table (t_0f3b232e, the Boy_HairFp pixel mismatch).
#
# Reference (sce-cgc 475, measured 2026-09-15, .local/probe-boyhair): in FP a
# constant exponent lowers as 0 -> the constant 1, 1 -> a copy, 2 -> MUL x, x
# (vectors too), 3 -> MUL then MUL, -1 -> RCP, -0.5 -> RSQ, 0.5 -> DIVSQR |x|, x,
# 4 / 8 / 0.25 / 0.125 -> LG2 with the multiply folded into its output scale
# then EX2, -2 / -4 / -8 / -0.25 the same with EX2 reading the negated log, and
# every other exponent (5, 6, 16, 32, 1.5, 0.75, -3, a variable) the LG2 / MUL
# / EX2 chain.  In VP: 2 -> MUL, 3 -> MUL MUL, -1 -> RCP, 0.5 -> RSQ + RCP.
#
# The integer and root rows carry VALUE, not shape: LG2 of a NEGATIVE base is
# NaN, so the uniform chain we used to emit painted NaN wherever
# pow(dot(t, h), 2) had a negative dot - Boy_HairFp mismatched the reference
# on 1369 of 4096 pixels for exactly that.  The checker executes our ucode on
# a negative-base input against an independent expectation, keeps a control
# proving it sees NaN, decodes the LG2 scale bits for the shape rows, and the
# byte-twin rows below are pairs the reference also emits identically.
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

scratch_root="${TMPDIR:-$repo_root/.local/tmp}"
mkdir -p "$scratch_root"
work="$(mktemp -d "$scratch_root/pow-constant-exponent-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

profile_of() { case "$1" in *_v) echo sce_vp_rsx ;; *) echo sce_fp_rsx ;; esac; }
ext_of()     { case "$1" in *_v) echo vpo ;; *) echo fpo ;; esac; }

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
    printf '  PASS  %-30s accepted: %s\n' "$name" "$why"
}

twin() {
    local a="$1" b="$2" why="$3"
    cmp -s "$work/$a.$(ext_of "$a")" "$work/$b.$(ext_of "$b")" || fail "$a and $b differ; the reference emits identical bytes ($why)"
    printf '  PASS  %-30s byte twin of %s: %s\n' "$a" "$b" "$why"
}

accept fp_pow_int_exponents_f      "pow(x,2) pow(y,3) pow(z,-1) pow(w,1) on a negative base"
accept fp_pow_int_exponents_twin_f "x*x, y*(y*y), 1/z, w"
accept fp_pow_zero_exponent_f      "pow(x,0) is 1 even for a negative x"
accept fp_pow_vec_two_f            "pow(float3, 2) is one MUL"
accept fp_pow_vec_two_twin_f       "v*v"
accept fp_pow_roots_f              "pow(z,0.5) pow(w,-0.5)"
accept fp_pow_roots_twin_f         "sqrt(z) rsqrt(w)"
accept fp_pow_nested_strand_f      "Boy_HairFp's pow(1 - pow(d,2), 2) with a negative d"
accept fp_pow_nested_strand_twin_f "the same with s = 1 - d*d; s*s"
accept fp_pow_chain_control_f      "exp2(2*log2(x)): the old chain, spelled out, as the NaN control"
accept fp_pow_scaled_four_f        "pow(z,4) folds the multiply into LG2's output scale"
accept fp_pow_scaled_eighth_f      "pow(z,0.125) uses the /8 output scale"
accept fp_pow_scaled_neg_two_f     "pow(z,-2) uses the x2 scale and a negated EX2 source"
accept fp_pow_chain_five_f         "pow(z,5) keeps the LG2 / MUL / EX2 chain"
accept fp_pow_variable_f           "pow(z,w) keeps the chain"
accept fp_pow_uniform_exponent_f   "pow(t.z, u.w) and pow(t.xyz, u.w): a uniform exponent reads ITS lane, w, not x"
accept fp_pow_uniform_root_f       "pow(u.y, 0.5) and pow(-u.z, 0.5): a uniform base in both DIVSQR slots, ONE inline block"
accept fp_pow_uniform_root_twin_f  "sqrt(u.y) spelling of the first lane"
accept fp_sqrt_uniform_f           "sqrt(u.y) alone: the pre-existing two-block emission"
accept vp_pow_mul_v                "vertex profile: pow(u.x,2) pow(u.y,3) pow(u.w,1) - lanes y and w, not x (the old path read lane x for every base)"
accept vp_pow_mul_twin_v           "u.x*u.x, u.y*(u.y*u.y), u.w"
accept vp_pow_rcp_root_v           "vertex profile: pow(u.z,-1) is RCP, pow(u.z,0.5) is RSQ + RCP"
accept vp_pow_rcp_root_twin_v      "1/u.z, sqrt(u.z)"

cp "$work/fp_pow_chain_control_f.fpo" "$work/control-chain.fpo"
python3 "$script_dir/pow_constant_check.py" "$work" || fail "pow_constant_check reported a defect"

twin fp_pow_int_exponents_f fp_pow_int_exponents_twin_f "integer exponents are the multiply / reciprocal spellings"
twin fp_pow_vec_two_f       fp_pow_vec_two_twin_f       "a vector squared is one MUL"
twin fp_pow_roots_f         fp_pow_roots_twin_f         "half exponents are sqrt / rsqrt"
twin fp_pow_uniform_root_f  fp_pow_uniform_root_twin_f  "pow(u, 0.5) is sqrt(u) on a uniform base too"
twin vp_pow_mul_v           vp_pow_mul_twin_v           "vertex integer exponents"
twin vp_pow_rcp_root_v      vp_pow_rcp_root_twin_v      "vertex reciprocal and root exponents"

# Self-check: a compiler that never writes must fail the accept rows even
# beside a seeded scratch directory.
if [[ -z "${POW_CONSTANT_SELFCHECK:-}" ]]; then
    stub="$work/never-writes.sh"
    printf '#!/usr/bin/env bash\nexit 0\n' >"$stub"
    chmod +x "$stub"
    seeded_root="$(mktemp -d "$scratch_root/pow-constant-seed.XXXXXX")"
    seeded="$(mktemp -d "$seeded_root/pow-constant-exponent-test.XXXXXX")"
    cp "$work"/*.fpo "$work"/*.vpo "$seeded/"
    control_rc=0
    POW_CONSTANT_SELFCHECK=1 TMPDIR="$seeded_root" bash "$0" "$stub" >"$work/never-writes.log" 2>&1 || control_rc=$?
    rm -rf "$seeded_root"
    [[ $control_rc -eq 1 ]] || { cat "$work/never-writes.log" >&2; fail "self-check: the never-writes stub exited $control_rc, not 1"; }
    grep -q "compiler wrote no container" "$work/never-writes.log" || { cat "$work/never-writes.log" >&2; fail "self-check: the never-writes stub failed for another reason"; }
    printf '  PASS  self-check: a compiler that never writes fails the accept rows\n'
fi

printf 'PASS: pow-constant-exponent-test\n'
