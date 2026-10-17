#!/usr/bin/env bash
# THE COMMA OPERATOR: `(a, b, c)` evaluates a and b for their effects and
# is c.
#
# The parser had no comma below assignment, so a parenthesised list refused
# with "Expected ')' after expression" - the gcm fw_fog fpshader row,
# `const float3 c = (0.2, 0.3, 0.4)`.  Every list context (arguments,
# constructor arguments, brace initialisers, declarator initialisers,
# parameter defaults) already parsed its items one assignment-expression at
# a time, so the comma lives in parseExpression only, and `f(a, b)` stays
# two arguments.
#
# Measured on the reference 2026-09-25 (.local/probe-frontend cm01-cm11), and
# every accept row here is a BYTE TWIN of the spelling that writes the
# operands out: the last-value fold; an ASSIGNMENT as left operand, which
# lands before the right operand reads it; an inout helper as left operand,
# which is called; varying left operands, dropped; commas in a for's init
# and increment, as a bare statement, in a return, in an array index, as a
# ternary's middle operand, and inside a call's argument parentheses.  The
# one refusal is the reference's: a comma in a declarator's initialiser
# without parentheses is a second declarator, and `0.7` is not one.
#
# Not here: an inout helper call as the left operand (the reference
# evaluates it, measured); this compiler does not inline helpers with
# out/inout parameters yet, a separate gap, so that row would measure it.
#
# Four more rows pin the static-loop split to INDEPENDENT extra parts: the
# reference accepts a for whose comma parts write the bound or the
# induction, or read the induction in the increment; this compiler refuses
# those by name rather than unrolling them wrongly (a review finding
# against the first split).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-comma-operator.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
decoder="$repo_root/tests/shader-compiler/fp_sources.py"

refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

compile() {   # <stem> -> rc; container at $work/<stem>.bin
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    rm -f "$work/$stem.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.bin" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    refusal_status "$rc" "$stem"
    return "$rc"
}

accepts() {
    if ! compile "$1"; then
        head -n 4 "$work/$1.log" >&2
        fail "$2 did not compile; the reference accepts it"
    fi
    [[ -s "$work/$1.bin" ]] || fail "$2 exited 0 but wrote no container"
}

twin() {   # <comma stem> <explicit stem> <what>
    accepts "$1" "$3"
    accepts "$2" "the explicit spelling of $3"
    if ! cmp -s "$work/$1.bin" "$work/$2.bin"; then
        python3 "$decoder" "$work/$1.bin" | sed 's/^/  comma:    /' >&2 || true
        python3 "$decoder" "$work/$2.bin" | sed 's/^/  explicit: /' >&2 || true
        fail "$3: the comma spelling and the explicit spelling produced different containers; the reference emits the same bytes"
    fi
}

twin fp_comma_last_f       fp_comma_last_ctl_f       "a const initialised from a comma list (fw_fog), against the scalar spelling"
twin fp_comma_assign_f     fp_comma_assign_ctl_f     "an assignment as the left operand"
twin fp_comma_varying_f    fp_comma_varying_ctl_f    "varying left operands"
twin fp_comma_for_f        fp_comma_for_ctl_f        "commas in a for's init and increment"
twin fp_comma_statement_f  fp_comma_statement_ctl_f  "a comma expression as a statement beside a two-declarator declaration"
twin fp_comma_call_arg_f   fp_comma_call_arg_ctl_f   "a parenthesised comma inside an argument list"
twin fp_comma_return_f     fp_comma_return_ctl_f     "a comma in a return"
twin fp_comma_index_f      fp_comma_index_ctl_f      "a comma in an array index"
twin fp_comma_ternary_f    fp_comma_ternary_ctl_f    "a comma as a ternary's middle operand"

# The side-effect rows must SHOW the effect: a pair that both dropped the
# left operand would agree.  The explicit assign twin writes uv.x*2 into
# lane y of the output; the comma spelling must carry the same MUL.
python3 - "$work/fp_comma_assign_f.bin" "$decoder" <<'PY'
import subprocess, sys
rows = subprocess.run([sys.executable, sys.argv[2], sys.argv[1]], capture_output=True, text=True, check=True).stdout
if "MUL" not in rows:
    raise SystemExit("FAIL: the assignment left operand's multiply is not in the ucode; the effect was dropped\n" + rows)
print("comma-operator: the assignment in a left operand reaches the ucode")
PY

# The typed constant fold takes the right operand's lanes and shape.
rc=0
compile fp_comma_refuse_shape_f || rc=$?
[[ "$rc" -eq 1 ]] || fail "float4((float3(1,2,3), 0.5), 1) compiled; the reference refuses it (C1067)"
grep -qiE "components|too little" "$work/fp_comma_refuse_shape_f.log" || fail "the comma-shape refusal did not name the component count"

# A for-clause comma is split only when its extra parts are independent of
# the loop proof.  These four are legal Cg (the reference accepts each) and
# stay REFUSED here by name: a bound written in the init, the induction
# written twice in the init, an increment part reading the induction, and
# two increment parts stepping it.  A guard that saw them compile would be
# seeing the wrong count or value.
# Two more (review finding): an init part writing a name the induction's
# initial value reads (`n = 3, i = n`: the proof read n before the init
# ran) and an increment part writing the step (`i += step, step = 2`).
for stem in fp_comma_for_refuse_bound_f fp_comma_for_refuse_step_f fp_comma_for_refuse_reads_f fp_comma_for_refuse_double_f fp_comma_for_refuse_init_reads_f fp_comma_for_refuse_step_written_f; do
    rc=0
    compile "$stem" || rc=$?
    [[ "$rc" -eq 1 ]] || fail "$stem compiled; its comma parts are not independent of the loop proof and the unroller must not take them"
    [[ -s "$work/$stem.bin" ]] && fail "$stem refused but still wrote a container"
    grep -q "static expansion refused" "$work/$stem.log" || {
        head -n 3 "$work/$stem.log" >&2
        fail "$stem refused without naming the loop-split rule"
    }
done

rc=0
compile fp_comma_refuse_bare_init_f || rc=$?
[[ "$rc" -eq 1 ]] || fail "float v = 0.2, 0.7; compiled; the reference refuses it (a comma there starts a second declarator)"
[[ -s "$work/fp_comma_refuse_bare_init_f.bin" ]] && fail "the bare-initialiser comma refused but still wrote a container"

printf 'comma-operator-test: PASS\n'
