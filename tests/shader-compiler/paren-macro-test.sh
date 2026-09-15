#!/usr/bin/env bash
# t_7cc742ed: A MACRO IS FUNCTION-LIKE ONLY WHEN '(' IMMEDIATELY FOLLOWS THE
# NAME.  With a space or a tab between them the parenthesis is the first
# character of an OBJECT-LIKE body.  Measured against sce-cgc 475.
#
# WHAT WENT WRONG.  processDefine matched
#     #\s*define\s+(\w+)(\s*\(([^)]*)\))?\s*(.*)
#                        ^^^
# so `#define fPI<TAB><TAB>(3.14159f)` was read as a function-like macro whose
# single parameter is named "3.14159f".  A bare fPI then expanded to nothing
# and eighteen SDK shaders died on "use of undeclared identifier 'fPI'".  It is
# not tab-specific and not fPI-specific: ANY object-like macro whose body
# starts with '(' was lost.
#
# WHY THIS GUARD CARRIES THE WHOLE SLICE.  No row in either census population
# closes.  The sixteen SDK rows that were blocked on fPI advance to a
# different blocker (a loop inside an inlined helper, which is not this
# slice's), and no tracked fixture uses the shape at all - the tracked census
# is unchanged in every column.  So there is no corpus row that would go red if
# this regressed, and these fixtures are the only thing standing between the
# rule and a silent return to dropping the macro.
#
# THE ROWS, and why each is here:
#
#   TWINS       every accepted shape must byte-equal its HAND-EXPANDED
#               spelling.  Acceptance alone would pass for a compiler that
#               expanded the macro to something else entirely.
#   BODY ROW    `#define K (x) (2.0)` is the one that proves the rule changes
#               what the BODY IS rather than merely accepting more.  The
#               reference expands it and then fails on the undefined x
#               (C1008).  Ours fails inside the expanded text too.  The
#               assertion is that the refusal does NOT name K - that was the
#               old behaviour and it means the macro was never defined.
#   C0105       the two no-space failures.  `#define K(3.14159f)` and
#               `#define K(` are function-like definitions whose parameter
#               list cannot be parsed, and the reference rejects the
#               DEFINITION.  Separate code paths: the first matches the
#               optional group, the second does not match it at all.
#   CONTROLS    a real function-like macro, called with and without a space,
#               with spaces inside the parameter list, with no parameters, and
#               an empty body - none of which may change.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-paren-macro.XXXXXX")"
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
        || fail "$3: refused, but not for the named reason - wanted /$2/, got: $(tr -d '\r' < "$work/$1.log" | grep -m1 -iE 'error|expected' | cut -c1-80)"
}

refuse_not_saying() {  # <stem> <pattern> <what>
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$3: expected exit 1, got $rc"
    [[ ! -e "$work/$1.fpo" ]] || fail "$3: refused but still wrote a container"
    if grep -qE "$2" <(tr -d '\r' < "$work/$1.log"); then
        fail "$3: the refusal still names the macro - it was never defined"
    fi
}

same() {  # <a> <b> <what>
    cmp -s "$work/$1.fpo" "$work/$2.fpo" \
        || fail "$3: $1 and $2 differ ($(stat -c%s "$work/$1.fpo") vs $(stat -c%s "$work/$2.fpo") bytes)"
    printf '  twin ok: %s\n' "$3"
}

P=fp_paren_macro

# ---- an object-like body that starts with '(' ----
accept ${P}_f          "a parenthesised object-like body, space before the ("
accept ${P}_twin_f     "the same body written out"
same   ${P}_f ${P}_twin_f "(3.14159f) expands to the body, not to nothing"

accept ${P}_tab_f      "the SDK's spelling, a TAB before the ("
same   ${P}_tab_f ${P}_twin_f "a tab separates exactly as a space does"

accept ${P}_guard_f    "the SDK's exact #ifndef/#define/#endif shape"
same   ${P}_guard_f ${P}_twin_f "the guard does not change the definition"

accept ${P}_nested_f      "nested parentheses in the body"
accept ${P}_nested_twin_f "that body written out"
same   ${P}_nested_f ${P}_nested_twin_f "the whole body survives, not just up to the first )"

accept ${P}_two_f      "a body that starts with ( and does not end with the match"
accept ${P}_two_twin_f "that body written out"
same   ${P}_two_f ${P}_two_twin_f "(1.0)*(0.5) is one body"

# ---- the cell that proves the BODY changed ----
refuse_not_saying ${P}_bodyexpand_refuse_f "identifier 'K'" \
    "#define K (x) (2.0) is OBJECT-like: the refusal must come from inside the expanded body"

# ---- a parameter list that does not parse is an error in the DEFINITION ----
refuse_saying ${P}_c0105_refuse_f      "C0105" "#define K(3.14159f): not a parameter name"
refuse_saying ${P}_c0105_open_refuse_f "C0105" "#define K( : unterminated parameter list"

# ---- object-like bodies that fail later, and must NOT become C0105 ----
refuse_not_saying ${P}_lonparen_refuse_f    "C0105" "#define K ( is a body, not a bad definition"
refuse_not_saying ${P}_emptyparens_refuse_f "C0105" "#define K () is a body, not a bad definition"

# ---- a form feed or vertical tab is INVALID INPUT, not a separator ----
# These are the cells codex found on the first revision: dropping \s* from the
# separator made `#define K<FF>(1.0)` object-like, so it ACCEPTED where the
# reference refuses.  NOT "anywhere in the source" - that was my first wording
# and it is wrong, as codex made me qualify: the reference refuses the
# character where it REACHES THE TOKEN STREAM.  A form feed in a comment, in a
# macro body that is never expanded, or in an argument bound to a parameter
# the body never names, is accepted - the accept rows below are those cases.
# The general rule is carded (t_a9273df1) and these rows pin the acceptance
# this slice introduced.
# The reference says "error C0000: syntax error, unexpected $undef"; we say
# "unknown character", because the character reaches our lexer as an unknown
# token rather than a bad expression.  Both refuse and write nothing, and the
# row pins OUR class - a divergence in wording, named here rather than papered
# over by matching on something vaguer.
refuse_saying ${P}_formfeed_refuse_f      "unknown character" "a form feed between the name and the body"
refuse_saying ${P}_vtab_refuse_f          "unknown character" "a vertical tab in the same place"
refuse_saying ${P}_formfeed_name_refuse_f "C0105" "a form feed BEFORE the name is the other code"

# WHERE THE RULE LIVES: the character is an error exactly where it is
# TOKENISED.  These three are the whole statement, and the middle one is why
# the check is not in the #define handler.
refuse_saying ${P}_formfeed_code_refuse_f "unknown character" "a form feed in ordinary code (the reference refuses; we used to accept)"
accept ${P}_formfeed_unused_f  "a form feed in a macro body that is NEVER expanded"
accept ${P}_formfeed_comment_f "a form feed inside a comment"

# WHEN the body reaches the token stream, in the two places where "used" is
# subtler than it looks.  Each accept row has the twin that does expand, so
# neither can pass on a compiler that simply stopped checking.
accept        ${P}_ff_named_not_called_f "a function-like NAME not followed by ( is not a use"
refuse_saying ${P}_ff_invoked_refuse_f   "unknown character" "the same macro actually invoked"
accept        ${P}_ff_unused_arg_f       "an argument bound to a parameter the body never names"
refuse_saying ${P}_ff_used_arg_refuse_f  "unknown character" "the same argument where the body DOES name it"

# A ## OPERAND TAKES THE RAW ARGUMENT - decided per OCCURRENCE, not per
# parameter. The mixed row is the one that forces it: the same parameter
# pasted AND plain, which the reference refuses while accepting every
# paste-only shape above it.
accept        ${P}_paste_left_f   "BAD as the LEFT operand of ##"
accept        ${P}_paste_right_f  "BAD as the RIGHT operand of ##"
accept        ${P}_paste_both_f   "BAD on both operands"
accept        ${P}_paste_nested_f "the paste as an argument to another macro"
refuse_saying ${P}_paste_mixed_refuse_f "unknown character" \
    "the SAME parameter pasted AND plain - per-parameter cannot give both answers"
accept        ${P}_paste_plain_f  "an ordinary paste, the control that ## still works"

# ACCEPTANCE IS NOT THE CLAIM - THE VALUE IS.  Every accepting paste row above
# is byte-compared against the hand-written literal it must expand to.  An
# accept-only row cannot tell "the paste produced 2.0" from "the paste produced
# something else that also compiled", and an accept-only row is exactly what
# let the pre-revision-4 per-parameter rule look right on every shape it did
# not happen to refuse (codex's condition on revision 4, endorsed by Fable).
# Measured, not assumed: all five equal the literal on the reference
# (43a4ac87280b) and here (29a658971f7e), so one twin is the control for all.
accept ${P}_paste_twin_f "the literal the pasted value expands to"
same   ${P}_paste_left_f   ${P}_paste_twin_f "the LEFT-operand paste is the value 2.0"
same   ${P}_paste_right_f  ${P}_paste_twin_f "the RIGHT-operand paste is the value 2.0"
same   ${P}_paste_both_f   ${P}_paste_twin_f "both operands pasted is the value 2.0"
same   ${P}_paste_nested_f ${P}_paste_twin_f "the nested paste is the value 2.0"
same   ${P}_paste_plain_f  ${P}_paste_twin_f "the ordinary paste is the value 2.0"

# ---- controls: none of this may change what a real function-like macro does ----
accept ${P}_fn_f       "a genuine function-like macro"
accept ${P}_fn_twin_f  "its expansion written out"
same   ${P}_fn_f ${P}_fn_twin_f "the function-like path is untouched"
accept ${P}_fncall_space_f "a space at the CALL"
same   ${P}_fncall_space_f ${P}_fn_f "a space at the call site is still a call"
accept ${P}_fnspace_params_f "spaces inside the parameter list"
same   ${P}_fnspace_params_f ${P}_fn_f "spaces inside the list do not matter"
accept ${P}_fnempty_f  "a function-like macro with no parameters"
accept ${P}_empty_f    "a macro with an empty body"

# SELF-CHECK: the refusal contract.  Two stubs - one that refuses with the
# right words while leaking a one-byte container, one that leaks an EMPTY file
# - must both make this script FAIL, and fail on the container assertion.  It
# runs last because each stub delegates every other row to the real compiler.
if [[ -z "${PAREN_MACRO_SELFTEST:-}" ]]; then
    for kind in onebyte empty; do
        stub="$work/leak-$kind.sh"
        cat > "$stub" <<LEAK
#!/usr/bin/env bash
out=""; prev=""
for x in "\$@"; do [[ "\$prev" == "--emit-container" ]] && out="\$x"; prev="\$x"; done
case " \$* " in
  *${P}_c0105_refuse_f.cg*)
    if [[ "$kind" == onebyte ]]; then printf 'x' > "\$out"; else : > "\$out"; fi
    echo 'error C0105: Syntax error in #define' >&2
    exit 1;;
esac
exec "$compiler" "\$@"
LEAK
        chmod +x "$stub"
        if PAREN_MACRO_SELFTEST=1 bash "${BASH_SOURCE[0]}" "$stub" > "$work/self-$kind.log" 2>&1; then
            fail "SELF-CHECK ($kind): a compiler that refuses with the right words but leaks a container PASSED this guard"
        fi
        grep -q "still wrote a container" "$work/self-$kind.log" \
            || fail "SELF-CHECK ($kind): the stub failed, but not on the container assertion - $(grep -m1 FAIL "$work/self-$kind.log" | cut -c1-80)"
    done
    printf '  self-check ok: a leaked container fails the guard, empty or not\n'
fi

echo "paren-macro: PASS"
