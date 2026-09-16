#!/usr/bin/env bash
# t_9e90fb38: THE `defined` OPERATOR, AND WHAT COUNTS AS DEFINED.
# Measured against sce-cgc 475.
#
# WHAT WENT WRONG.  evaluateExpression called expandMacros FIRST and applied
# two `defined` regexes afterwards, so the operand they captured was the
# EXPANDED token:
#     #define X 1
#     #if defined(X)      ->  defined(1)  ->  lookup of "1" fails  ->  FALSE
# The operator was therefore INVERTED for every object-like macro - it
# reported false exactly when the macro WAS defined.  No diagnostic and no
# refusal: the wrong arm of the conditional was simply compiled.  Four
# DeferredShading fragment shaders died on it, because mrt_configuration.h
# writes `#define EYE_SPACE_NORMALS` with an EMPTY body and
# normal_transformation.cgh tests it with `#elif defined (...)`; expanding
# first made that `defined ()`, which matched neither regex, so every arm of
# the chain was false and eyeNrmToViewSpace never existed.
#
# WHERE THE OPERATOR IS RECOGNISED, and why it cannot be anywhere else.
# Inside the expansion, before the hide-set test and before the macro lookup.
#   - not BEFORE expansion: `#define D defined` then `#if D(X)` is TRUE on the
#     reference, and the operator only exists once D has been replaced;
#   - not AFTER expansion: the expander rescans replacements, so by then D(X)
#     has already become defined(1) and the operand name is gone;
#   - before the macro lookup: `#define defined 9` does not shadow it.
#
# WHAT IS NOT PROTECTED.  A macro ARGUMENT is pre-expanded like any other text
# and the reference refuses what that produces - HAS(x)=defined(x) with
# HAS(X), and ID(x)=x with ID(defined(X)), are both C0105.  A blanket "protect
# every argument" would wrong-accept both and look like a fix.
#
# THE NEIGHBOUR THIS SLICE ALSO FIXES, and the rule that is NOT a name list.
# #ifdef tested macros.find() alone, so `#ifdef __LINE__` was TRUE here and
# FALSE on the reference.  Both spellings now share ONE predicate - but the
# predicate asks the BINDING, not the name:
#     pristine __LINE__/__FILE__/__DATE__/__TIME__   not defined, still expand
#     __CGC__ / __SCE_CGC__                          defined
#     #define __LINE__ 7                             defined, expands to 7
#     #undef __LINE__                                not defined, expands the
#                                                    line number again
#     #define __LINE__ 7 then #undef __LINE__        not defined, line number
#     #undef then #define                            defined
# A list of special names would have passed the first two rows and failed
# every other one.  It is the binding's provenance that decides, so the bit
# lives on the binding (codex found the hole; ruling by Fable).
#
# ONE CELL IS DELIBERATELY NOT HERE.  `#if defined(X) ? 1 : 0` is C0105 on the
# reference and accepted by us, because our ExprParser has a ternary and the
# reference #if grammar does not.  That is a property of the grammar, not of
# this operator; it is carded as t_ebe8f97e and this guard asserts nothing
# about it.  Completing the refusal list by deleting one operator would be
# this slice quietly becoming a grammar change.
#
# EVERY ACCEPTING ROW IS A VALUE ROW.  All of these compile either way - only
# the BRANCH TAKEN separates a fixed compiler from a broken one, so each is
# byte-compared against the literal it must equal.  An acceptance row would
# pass on the tip.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-defined.XXXXXX")"
trap 'rm -rf "$work"' EXIT

compile() {  # <stem> -> rc
    local stem="$1" rc=0
    rm -f "$work/$stem.fpo"
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" \
        "$shaders/$stem.cg" > "$work/$stem.log" 2>&1 || rc=$?
    printf '%s' "$rc"
}

accept() {  # <stem> <what>
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$1.log" >&2; fail "$2: expected exit 0, got $rc"; }
    [[ -s "$work/$1.fpo" ]] || fail "$2: exit 0 but wrote no container"
}

# A refusal is a verdict, a reason AND an absence.  `! -e`, not `! -s`: an
# empty output file is an output file.
refuse_saying() {  # <stem> <pattern> <what>
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$3: expected exit 1, got $rc"
    [[ ! -e "$work/$1.fpo" ]] || fail "$3: refused but still wrote a container"
    grep -qE "$2" <(tr -d '\r' < "$work/$1.log") \
        || fail "$3: refused, but not for the named reason"
}

# The whole point of the slice: WHICH BRANCH was taken.
branch() {  # <stem> <one|two> <what>
    accept "$1" "$3"
    cmp -s "$work/$1.fpo" "$work/fp_defined_$2_f.fpo" \
        || fail "$3: took the WRONG BRANCH - $1 does not equal fp_defined_$2_f"
    printf '  branch ok (%s): %s\n' "$2" "$3"
}

P=fp_defined_

# ---- the two literals every value row is compared against ----
accept ${P}one_f   "the literal of the true branch"
accept ${P}two_f   "the literal of the false branch"
accept ${P}seven_f "the literal an explicitly defined builtin must equal"
accept ${P}eight_f "the line number the reinsertion row must equal"
if cmp -s "$work/${P}one_f.fpo" "$work/${P}two_f.fpo"; then
    fail "the two branch literals compile to the SAME container - every branch row below would be vacuous"
fi

# ---- the operator ----
branch ${P}object_f     one "a defined object-like macro is DEFINED"
branch ${P}absent_f     two "an undefined name is not"
branch ${P}noparen_f    one "the parenthesis-free spelling"
branch ${P}spaces_f     one "whitespace around the operator and its parens"
branch ${P}not_f        one "negated, so a wrong answer shows as the other branch"
branch ${P}and_f        one "two operators in one expression"
branch ${P}arith_f      one "the operator yields the integer 1"
branch ${P}empty_body_f one "an EMPTY-BODIED macro - the SDK own shape"

# ---- where the operator comes from, and what outranks what ----
branch ${P}operator_from_macro_f one "the operator produced BY an expansion"
branch ${P}via_object_macro_f    one "operator and operand from one macro"
branch ${P}shadowed_f            two "a macro named defined does not shadow the operator"

# ---- the operand is a NAME, never a value ----
branch ${P}operand_not_expanded_f one "the operand is looked up, not evaluated"
branch ${P}operand_alias_f        one "the same, with the operator from a macro too"
branch ${P}functionlike_name_f    one "a function-like macro name survives defined(F)"

# ---- the other directives on the same evaluator ----
branch ${P}elif_f    one "#elif takes the identical path"
branch ${P}defined_f two "defined is not itself a macro"

# ---- expanding and being DEFINED are different things ----
branch ${P}builtin_line_f   two "__LINE__ expands but is not defined"
branch ${P}builtin_file_f   two "__FILE__ expands but is not defined"
branch ${P}builtin_date_f   two "__DATE__ expands but is not defined"
branch ${P}builtin_time_f   two "__TIME__ expands but is not defined"
branch ${P}builtin_cgc_f    one "__CGC__ is an ordinary predefined macro"
branch ${P}builtin_scecgc_f one "__SCE_CGC__ is too"
branch fp_ifdef_builtin_line_f two "#ifdef agrees: __LINE__ is not defined"
branch fp_ifdef_builtin_file_f two "#ifdef agrees: __FILE__ is not defined"
branch fp_ifdef_builtin_date_f two "#ifdef agrees: __DATE__ is not defined"
branch fp_ifdef_builtin_cgc_f  one "#ifdef agrees: __CGC__ is defined"

# ---- ...and the moment the SOURCE defines one, it IS defined ----
# Both spellings, all four driver-owned names, defined / undefined /
# defined-then-undefined / undefined-then-defined, plus the expansion value.
branch fp_defined_src_line_f       one "an explicit #define __LINE__ counts as defined"
branch fp_ifdef_src_line_f         one "#ifdef agrees for the source-defined __LINE__"
branch fp_defined_undef_line_f     two "#undef __LINE__ leaves it undefined"
branch fp_defined_src_undef_line_f two "#define then #undef __LINE__ is undefined again"
branch fp_defined_redef_line_f     one "#undef then #define __LINE__ is defined again"
branch fp_defined_value_line_f     seven "an explicit #define __LINE__ EXPANDS to the source value"
branch fp_defined_src_file_f       one "an explicit #define __FILE__ counts as defined"
branch fp_ifdef_src_file_f         one "#ifdef agrees for the source-defined __FILE__"
branch fp_defined_undef_file_f     two "#undef __FILE__ leaves it undefined"
branch fp_defined_src_undef_file_f two "#define then #undef __FILE__ is undefined again"
branch fp_defined_redef_file_f     one "#undef then #define __FILE__ is defined again"
branch fp_defined_value_file_f     seven "an explicit #define __FILE__ EXPANDS to the source value"
branch fp_defined_src_date_f       one "an explicit #define __DATE__ counts as defined"
branch fp_ifdef_src_date_f         one "#ifdef agrees for the source-defined __DATE__"
branch fp_defined_undef_date_f     two "#undef __DATE__ leaves it undefined"
branch fp_defined_src_undef_date_f two "#define then #undef __DATE__ is undefined again"
branch fp_defined_redef_date_f     one "#undef then #define __DATE__ is defined again"
branch fp_defined_value_date_f     seven "an explicit #define __DATE__ EXPANDS to the source value"
branch fp_defined_src_time_f       one "an explicit #define __TIME__ counts as defined"
branch fp_ifdef_src_time_f         one "#ifdef agrees for the source-defined __TIME__"
branch fp_defined_undef_time_f     two "#undef __TIME__ leaves it undefined"
branch fp_defined_src_undef_time_f two "#define then #undef __TIME__ is undefined again"
branch fp_defined_redef_time_f     one "#undef then #define __TIME__ is defined again"
branch fp_defined_value_time_f     seven "an explicit #define __TIME__ EXPANDS to the source value"

# ---- the reinsertion row, named on its own ----
# #undef does not stop a driver-owned name expanding; it only stops it
# counting as defined.  After #define then #undef, __LINE__ is back to the
# line number - and the use in that fixture is on line 8.
branch fp_defined_line_restored_f eight "after #define and #undef, __LINE__ expands the line again"

# ---- a skipped group condition is READ, never EVALUATED ----
# These four are the negatives.  They passed before this slice for the wrong
# reason - nothing in a condition could fail - and the first build of the fix
# turned two of them red.
branch ${P}skipped_group_f one "a malformed condition inside a skipped #if 0 group"
branch ${P}skipped_else_f  one "the same in the arm not taken - the #if arm IS taken, so the file is the true branch"
branch ${P}skipped_elif_f  one "the same in an #elif whose parent is already skipped"
branch ${P}skipped_ifdef_f       one "an #ifdef door into a skipped group, with a malformed #elif arm"
branch ${P}skipped_ifndef_f      one "the #ifndef door, the same"
branch ${P}skipped_nested_if_f   one "the #if door - the control that was already shut"
branch ${P}dead_elif_f     one "an #elif after a TAKEN arm is never evaluated"

# ---- malformed operands are an error in the directive ----
refuse_saying ${P}open_refuse_f            "C0105" "#if defined( : no operand and no close"
refuse_saying ${P}empty_operand_refuse_f   "C0105" "#if defined() : no operand"
refuse_saying ${P}number_operand_refuse_f  "C0105" "#if defined(1) : a number is not an identifier"
refuse_saying ${P}empty_condition_refuse_f "C0105" "an empty condition is an error, not false"

# ---- a macro argument is NOT protected ----
refuse_saying ${P}macro_argument_refuse_f  "C0105" "HAS(x)=defined(x) with HAS(X): the argument was expanded first"
refuse_saying ${P}inside_argument_refuse_f "C0105" "ID(defined(X)): the same, from inside the argument"

# ---- self-check: a compiler that refuses with the right words but leaks a
# container must not pass this guard ----
if [[ -z "${DEFINED_SELFTEST:-}" ]]; then
    for kind in onebyte empty; do
        stub="$work/leak-$kind.sh"
        {
          echo '#!/usr/bin/env bash'
          echo 'out=""; prev=""'
          echo 'for x in "$@"; do [[ "$prev" == "--emit-container" ]] && out="$x"; prev="$x"; done'
          echo 'case " $* " in'
          echo "  *${P}open_refuse_f.cg*)"
          if [[ "$kind" == onebyte ]]; then
              echo '    printf x > "$out";'
          else
              echo '    : > "$out";'
          fi
          echo "    echo 'error C0105: Syntax error in #if' >&2"
          echo '    exit 1;;'
          echo 'esac'
          echo "exec \"$compiler\" \"\$@\""
        } > "$stub"
        chmod +x "$stub"
        if DEFINED_SELFTEST=1 bash "${BASH_SOURCE[0]}" "$stub" > "$work/self-$kind.log" 2>&1; then
            fail "SELF-CHECK ($kind): a compiler that refuses with the right words but leaks a container PASSED this guard"
        fi
        grep -q "still wrote a container" "$work/self-$kind.log" \
            || fail "SELF-CHECK ($kind): the stub failed, but not on the container assertion"
    done
    printf '  self-check ok: a leaked container fails the guard, empty or not\n'
fi

echo "defined-operator: PASS"
