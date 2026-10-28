#!/usr/bin/env bash
# A TYPE WORD CAN BE DECLARED AS A NAME, AND THEN IT IS THAT NAME.
#
# `float3 half = float3(0, 0, 1);` - the PSGL jetstream vpFixedPipeline
# row - refused with "Expected ( after type in expression": the lexer
# keeps `half` a keyword, and the parser read every later `half` as the
# start of a constructor.
#
# Measured on the established compiler 2026-09-25 (.local/probe-frontend hk_*): `half`,
# `fixed`, `sampler2D`, `double` and the vector names can be declared as
# variables, parameters or file-scope globals, in both profiles, and the
# result is byte-identical to the same shader with an ordinary name; from
# the declaration on the word is the variable, so `half(0.5)` after it is
# C1105 "cannot call a non-function", and `float3 float3 = float3(0, 0,
# 1)` fails the same way because the name is taken before its initialiser
# is read.  `half3` stays a type beside a variable `half`.  `float`, `int`
# and `bool` are reserved words there: C0000 at the word.
#
# A typedef alias or a struct name is a type word too: a local declared
# under it shadows it the same way (`typedef A B; ... float B = 2.0;` reads
# the local on the established compiler, and `B - x` under `typedef float4 B` is a
# subtraction, not a cast).
#
# The parser records a declared type word as shadowed and reads it as an
# identifier while in scope: a function's parameters and locals end with
# the function, a file-scope variable persists.  Struct members are names
# only behind a `.`, so they shadow nothing.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-type-words-as-names.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

refusal_status() {
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

compile() {
    local stem="$1" profile rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    case "$stem" in *_v) profile=sce_vp_rsx ;; *) profile=sce_fp_rsx ;; esac
    rm -f "$work/$stem.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" --emit-container "$work/$stem.bin" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    refusal_status "$rc" "$stem"
    return "$rc"
}

accepts() {
    if ! compile "$1"; then
        head -n 4 "$work/$1.log" >&2
        fail "$2 did not compile; the established compiler accepts it"
    fi
    [[ -s "$work/$1.bin" ]] || fail "$2 exited 0 but wrote no container"
}

twin() {   # <shadow stem> <control stem> <what>
    accepts "$1" "$3"
    accepts "$2" "the control of $3"
    cmp -s "$work/$1.bin" "$work/$2.bin" \
        || fail "$3: the shadowing spelling and its control produced different containers; the established compiler emits the same bytes"
}

# A parameter's or a global's NAME is recorded in the container, so those
# rows compare the ucode region only (the established compiler containers differ the
# same way: b30dffeb vs 527b4be4 for the parameter pair).
ucode_twin() {   # <shadow stem> <control stem> <what>
    accepts "$1" "$3"
    accepts "$2" "the control of $3"
    python3 - "$work/$1.bin" "$work/$2.bin" "$repo_root/tests/shader-compiler" <<'PY'
import sys
sys.path.insert(0, sys.argv[3])
from uniform_container_check import Container
a = Container(open(sys.argv[1], "rb").read()); b = Container(open(sys.argv[2], "rb").read())
if a.blob[a.ucode:a.ucode + a.ucode_size] != b.blob[b.ucode:b.ucode + b.ucode_size]:
    raise SystemExit("FAIL: the ucode differs between the shadowing spelling and its control")
PY
}

refuses() {   # <stem> <what> <text>
    local rc=0
    compile "$1" || rc=$?
    [[ "$rc" -eq 1 ]] || fail "$2 compiled; the established compiler refuses it"
    [[ -s "$work/$1.bin" ]] && fail "$2 refused but still wrote a container"
    grep -qiE -- "$3" "$work/$1.log" || {
        head -n 4 "$work/$1.log" >&2
        fail "$2 refused without naming '$3'"
    }
    return 0
}

twin fp_typeword_half_f            fp_typeword_ctl_f             "float3 half (jetstream)"
twin fp_typeword_fixed_f           fp_typeword_ctl_f             "float3 fixed"
twin fp_typeword_sampler2D_f       fp_typeword_ctl_f             "float3 sampler2D"
twin fp_typeword_half_then_half3_f fp_typeword_ctl_f             "half3 as a type beside a variable half"
ucode_twin fp_typeword_half_param_f  fp_typeword_half_param_ctl_f  "a parameter named half"
ucode_twin fp_typeword_half_global_f fp_typeword_half_global_ctl_f "a file-scope variable named half"
# A FUNCTION named after a type word is called, not read as a cast (review
# finding: the name shadow was dropped with the function's locals, so the
# later call parsed as a constructor and compiled uv.x instead of 2*uv.x).
twin fp_typeword_function_f        fp_typeword_function_ctl_f     "float half(float x) called afterwards"
# A name declared in an inner block shadows a type only inside it (review
# finding: the shadow outlived its block and hid the struct tag).
twin fp_typeword_block_shadow_f    fp_typeword_block_shadow_ctl_f "{ float B = uv.x; } then B b"
twin fp_typeword_for_init_shadow_f fp_typeword_for_init_shadow_ctl_f "for (int B = 0; ...) {} then B b"
twin fp_typeword_scope_ends_f      fp_typeword_scope_ends_ctl_f  "a helper's local half does not shadow the type in the entry"
twin vp_typeword_half_v            vp_typeword_half_ctl_v        "float3 half in the vertex profile"
accepts fp_typeword_member_half_f "a struct member named half beside the half constructor"
twin fp_typeword_alias_local_f     fp_typeword_alias_local_ctl_f     "a local named after a typedef alias"
twin fp_typeword_builtin_alias_local_f fp_typeword_builtin_alias_local_ctl_f "a local named after a builtin alias, then subtracted"
twin fp_typeword_builtin_alias_paren_f fp_typeword_builtin_alias_local_ctl_f "(B) - x under a shadowed builtin alias is not a cast"

refuses fp_typeword_refuse_float_f        "float3 float"                    "identifier|Expected"
refuses fp_typeword_refuse_int_f          "float3 int"                      "identifier|Expected"
refuses fp_typeword_refuse_bool_f         "float3 bool"                     "identifier|Expected"
refuses fp_typeword_refuse_call_after_f   "half(0.5) after float3 half"     "half"
refuses fp_typeword_refuse_float3_ctor_f  "float3 float3 = float3(0, 0, 1)" "float3"

printf 'type-words-as-names-test: PASS\n'
