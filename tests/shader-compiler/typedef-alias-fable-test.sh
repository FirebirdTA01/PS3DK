#!/usr/bin/env bash
# A TYPEDEF WHOSE TARGET IS AN EXISTING TYPE IS AN ALIAS, AND AN ALIAS IS
# RESOLVED IN THE PARSER.
#
# `typedef struct { ... } Name;` was already handled by naming the struct
# after the typedef, and that works precisely because the struct has no
# other name.  Every other spelling - `typedef A B;`, `typedef struct Foo
# Bar;`, `typedef struct Tag { ... } Name;` used through Tag, `typedef float4
# vec4_t;`, `typedef float2 taps_t[2];` - has an EXISTING type to point at,
# and nothing recorded the pointer: the parser put the name in its
# type-name set, so a use parsed as a struct reference called B, and
# semantic analysis then found no struct called B ("unknown type for
# parameter").  The DeferredShading light passes fail this way: each pass
# writes `typedef dir_light_params_t light_params_t;` over the header's
# anonymous-struct typedef.
#
# The fix keeps an alias table (name -> TypeNode) in the parser and hands
# every later use a COPY of the aliased node, so semantic analysis, struct
# layout and the IR builder's cast-target name compare never see the alias.
#
# Every row below was measured on the reference on 2026-09-25 (probe dir
# .local/probe-decl, 33 cells).  The accept rows are TWINS: the alias
# spelling must produce the same container as the direct spelling, which
# is what the reference does for every one of them, VP included.  The
# refusals are the reference's own: a keyword-less alias of a struct not
# yet declared, an unknown target, any second definition of the name
# (an identical re-typedef included - C11 allows that, the reference does
# not, C1002), and a forward `typedef struct Foo Bar;` whose Foo is never
# defined but IS used (C1313).  A forward tag that is never used is
# accepted, as on the reference.
#
# One twin has its OWN control: fp_typedef_tag_and_name_f routes i.col
# through a helper's parameter, and on this compiler a struct member that
# reaches the output only that way loses its varying record in the
# container (ucode identical, record gone; the reference keeps it and its
# bytes equal the direct spelling's).  That is a reflection gap of the call
# path, present without any typedef, so the twin is the same helper shape
# under the tag alone.
#
# What this does NOT cover: a UNIFORM struct parameter through an alias -
# `uniform light_t L`, or `light_t L` whose members are declared uniform,
# the DeferredShading light-pass shape.  The alias resolves (the parser no
# longer refuses the parameter), but the lowering then treats the struct
# parameter as a varying input and refuses "unsupported input semantic";
# the parent does the same for the tag spelling, and the reference accepts
# all three spellings (member-uniform, parameter-uniform, aliased) with one
# container.  That is the light passes' NEXT blocker and has its own card.
#
# Nor the alias as a struct MEMBER type, a struct-typed local copied from a
# parameter, or a struct returned through a helper: the parent refuses all
# three under the tag spelling too ("did not lower completely"), so a twin
# there would measure the struct-copy gap, not the alias.
#
# Nor a typedef inside a function body.  The
# reference accepts it and scopes it to the body (measured: a use outside
# the body is a syntax error there); no reference-SDK source writes one,
# and this compiler still refuses it at the statement.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-typedef-alias.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# A refusal is exit 1 EXACTLY.  124 is a timeout and >=128 is a signal, and
# either satisfies "did not exit 0" while meaning the compiler never reached
# the decision this guard is about.
refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

compile() {   # <stem> -> rc; container at $work/<stem>.bin, log at .log
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

accepts() {   # <stem> <what>
    if ! compile "$1"; then
        head -n 4 "$work/$1.log" >&2
        fail "$2 did not compile; the reference accepts it"
    fi
    [[ -s "$work/$1.bin" ]] || fail "$2 exited 0 but wrote no container"
}

twin() {   # <alias stem> <direct stem> <what>
    accepts "$1" "$3"
    accepts "$2" "the direct spelling of $3"
    cmp -s "$work/$1.bin" "$work/$2.bin" \
        || fail "$3: the alias spelling and the direct spelling produced different containers; the reference emits the same bytes for both"
}

refuses() {   # <stem> <what> <text the diagnostic must contain (grep -E)>
    local rc=0
    compile "$1" || rc=$?
    [[ "$rc" -eq 1 ]] || fail "$2 compiled; the reference refuses it"
    [[ -s "$work/$1.bin" ]] && fail "$2 refused but still wrote a container"
    grep -qiE -- "$3" "$work/$1.log" || {
        head -n 4 "$work/$1.log" >&2
        fail "$2 refused without naming '$3'; a refusal that does not say which rule made it is a dead end"
    }
    return 0
}

# --- the alias spellings, each a twin of the direct one ------------------
twin fp_typedef_alias_of_anon_f     fp_typedef_anon_ctl_f      "typedef A B; of an anonymous-struct typedef (the light-pass shape)"
twin fp_typedef_alias_of_tag_f      fp_typedef_anon_ctl_f      "typedef Foo Bar; of a tagged struct"
twin fp_typedef_struct_tag_alias_f  fp_typedef_anon_ctl_f      "typedef struct Foo Bar;"
twin fp_typedef_tag_and_name_f      fp_typedef_tag_and_name_ctl_f "typedef struct Tag {...} Name; with Tag and Name both used"
twin fp_typedef_chain_f             fp_typedef_anon_ctl_f      "an alias of an alias"
twin fp_typedef_struct_fwd_f        fp_typedef_anon_ctl_f      "a forward typedef struct Foo Bar; defined later"
twin fp_typedef_builtin_f           fp_typedef_builtin_ctl_f   "aliases of float4 and half3"
twin fp_typedef_array_f             fp_typedef_array_ctl_f     "typedef float2 taps_t[2];"
twin fp_typedef_cast_fill_f         fp_typedef_cast_fill_ctl_f "(O)0 into a variable declared through an alias of O"
twin vp_typedef_alias_v             vp_typedef_ctl_v           "an alias on a vertex input struct"
twin fp_typedef_output_f            fp_typedef_output_ctl_f    "the alias as the entry return type, a helper parameter and a local"
accepts fp_typedef_fwd_unused_f          "a forward tag that is never defined and never used"

# The twins have to be twins of SOMETHING: the direct spelling must reach
# the ucode, or a pair that both emitted an empty program would agree.
python3 - "$work/fp_typedef_anon_ctl_f.bin" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
if len(data) < 64:
    raise SystemExit("FAIL: the control container is %d bytes; nothing to twin against" % len(data))
print("typedef-alias: control container %d bytes" % len(data))
PY

# --- the reference's refusals --------------------------------------------
refuses fp_typedef_refuse_before_def_f  "typedef Foo Bar; before struct Foo"     "unknown type name 'Foo'"
refuses fp_typedef_refuse_unknown_f     "typedef Nope Bar;"                       "unknown type name 'Nope'"
refuses fp_typedef_refuse_dup_same_f    "the same typedef written twice"         "'B' is already defined"
refuses fp_typedef_refuse_dup_diff_f    "a name typedef'd to two types"          "'B' is already defined"
refuses fp_typedef_refuse_var_then_alias_f  "a typedef of a name a global already owns"   "'B' is already defined"
refuses fp_typedef_refuse_alias_then_var_f  "a global variable named like an earlier alias" "already defined|redefinition of variable 'B'"
refuses fp_typedef_refuse_alias_then_func_f "a function named like an earlier alias"        "already defined|redefinition|'B'"
refuses fp_typedef_refuse_fwd_used_f    "a forward tag used but never defined"   "unknown type"

# A refusal that reports the fault must report it ONCE.  The old path
# reported an unknown typedef target at the target and then again at the
# typedef's name, because the loop re-read the name as a new declaration.
count=$(grep -c "error" "$work/fp_typedef_refuse_unknown_f.log" || true)
[[ "$count" -le 2 ]] || {
    cat "$work/fp_typedef_refuse_unknown_f.log" >&2
    fail "typedef Nope Bar; produced $count error lines; the fault is one line plus the summary"
}

printf 'typedef-alias-test: PASS\n'
