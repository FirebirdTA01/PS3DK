#!/usr/bin/env bash
# A FUNCTION REACHED FROM THE SELECTED ENTRY MAY NOT CARRY A RETURN SEMANTIC.
#
# The reference has a diagnostic for this and ITS TEXT IS WRONG ABOUT ITS RULE:
#
#     error C5122: semantics not allowed on functions other than the entry
#     function
#
# Taken literally that forbids two shapes the reference ACCEPTS.  Measured on
# sce-cgc 475, sce_fp_rsx, every row below compiled with -e beta:
#
#     called helper, RETURN semantic                    REFUSE
#     called helper, RETURN + PARAMETER semantics       REFUSE
#     called helper, PARAMETER semantic only            ACCEPT
#     declared but never called, full semantics         ACCEPT
#     called only by a function the entry never calls   ACCEPT
#     transitively reachable through an intermediate    REFUSE
#     called only inside `if (false)`                   REFUSE
#     called from a DEFAULT-ARGUMENT expression         ACCEPT
#
# So the rule is transitive reachability from the SELECTED entry over the
# SYNTACTIC call graph, without branch pruning, and default-argument
# expressions create no edge.  ONLY THE ACCEPTANCE TABLE IS THE RULE; the
# diagnostic is the compiler's account of it (t_61109061).
#
# WHY THE CONTAINER ROWS EXIST.  The check walks the AST for call edges.  A
# walker that misses ONE node kind under-approximates reachability, and
# under-approximating means ACCEPTING a program the reference refuses - this
# check's own defect, reintroduced by its fix, and invisible to any fixture
# that happens to use a handled kind.  There is one row per container: if,
# for, while, do-while, ternary, return, cast, constructor argument, index,
# member-access base, binary operand, unary operand, DECLARATION INITIALISER
# and BARE EXPRESSION STATEMENT.  The last two were missing from the first
# draft of this guard - the walker handled them, nothing proved it, and an
# audit of the fixture list against the walker's switch is what found them.
# Auditing the guard against the code is a separate act from writing either.
#
# TWO ARMS OF THE WALK CANNOT BE WITNESSED HERE, and that is measured rather
# than an omission: the reference's parser rejects `switch` outright -
# "error C0000: syntax error, unexpected reserved word \"switch\"" - with or
# without a call in it, so no program containing a switch reaches the C5122
# rule at all.  The Switch and Case arms are handled in the walker for
# completeness and because our own parser accepts them; they have no reference
# verdict to pin, so they get no row rather than a row asserting our own
# behaviour as if it were the rule.
#
# EVERY REFUSE ROW ASSERTS THE DIAGNOSTIC TEXT, not just a non-zero status.
# Three of the containers are refused on a PRISTINE PARENT for unrelated
# reasons - while/do-while reach "no instructions emitted" and the index row
# reaches "local array dynamic indexing is not supported" - so a status-only
# assertion would read green on the parent for the wrong reason.  With the
# text asserted, those three are red at a DISTINGUISHABLE wrong diagnostic
# before this lands and red at the right one after, and the row still fails if
# some later slice fixes the loop bucket and the shape starts refusing for a
# third reason.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || { echo "usage: $0 <rsx-cg-compiler>" >&2; exit 2; }
[[ -x "$compiler" ]] || { echo "FAIL: not executable: $compiler" >&2; exit 1; }

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# The C5122-class text this compiler emits.  Asserted on every refusal.
needle="semantics not allowed on functions other than the entry function"

compile() {  # <stem> -> rc, stdout+stderr in $work/<stem>.log
    local stem="$1"
    set +e
    "$compiler" -p sce_fp_rsx -e beta --emit-container "$work/$stem.fpo" \
        "$shaders/$stem.cg" > "$work/$stem.log" 2>&1
    local rc=$?
    set -e
    printf '%s' "$rc"
}

refuse() {  # <stem> <what>
    local stem="$1" what="$2" rc
    rc="$(compile "$stem")"
    [[ "$rc" -eq 1 ]] || fail "$what: expected exit 1, got $rc"
    tr -d '\r' < "$work/$stem.log" | grep -qF "$needle" \
        || fail "$what: exit 1 but NOT for the semantics rule - $(tr -d '\r' < "$work/$stem.log" | grep -m1 -E 'error|nv40' | cut -c1-90)"
    [[ ! -s "$work/$stem.fpo" ]] || fail "$what: refused but still wrote a container"
    echo "  refuse ok: $what"
}

accept() {  # <stem> <what>
    local stem="$1" what="$2" rc
    rc="$(compile "$stem")"
    [[ "$rc" -eq 0 ]] || fail "$what: expected exit 0, got $rc - $(tr -d '\r' < "$work/$stem.log" | grep -m1 -E 'error|nv40' | cut -c1-90)"
    [[ -s "$work/$stem.fpo" ]] || fail "$what: exit 0 but wrote no container"
    echo "  accept ok: $what"
}

# ---- the fourteen containers, one per AST node kind that can hold a call ----
refuse fp_c5122_if_f        "if branch"
refuse fp_c5122_for_f       "for body"
refuse fp_c5122_while_f     "while body"
refuse fp_c5122_dowhile_f   "do-while body"
refuse fp_c5122_ternary_f   "ternary arm"
refuse fp_c5122_return_f    "return expression"
refuse fp_c5122_cast_f      "cast operand"
refuse fp_c5122_ctor_f      "constructor argument"
refuse fp_c5122_index_f     "index expression"
refuse fp_c5122_member_f    "member-access base"
refuse fp_c5122_binary_f    "binary operand"
refuse fp_c5122_unary_f     "unary operand"
refuse fp_c5122_declinit_f  "declaration initialiser"
refuse fp_c5122_exprstmt_f  "bare expression statement"

# ---- the two rows that pin the NOTION rather than the walk ----
refuse fp_c5122_transitive_f "transitive: beta -> delta -> gamma"
refuse fp_c5122_deadbranch_f "syntactic: reached only inside if (false)"

# ---- the PROTOTYPE cells: which DECLARATION carries the judged semantic ----
# Measured, and the four cells disagree - so neither "judge the definition" nor
# "judge either declaration" reproduces the reference.  The semantic that is
# JUDGED is the one on the FIRST DECLARATION in source order; the body that is
# WALKED is the DEFINITION's.  The bare-prototype row is the one that forbids
# the natural fix: a walk that followed the prototype and judged the definition
# would REFUSE it, and the reference accepts it.
refuse fp_c5122_proto_transitive_f   "transitive through a bodyless prototype"
refuse fp_c5122_proto_both_f         "prototype :COLOR + definition :COLOR"
refuse fp_c5122_proto_sem_def_bare_f "prototype :COLOR + definition bare"
refuse fp_c5122_proto_def_first_f    "definition :COLOR first, prototype after"
accept fp_c5122_proto_bare_def_sem_f "prototype bare + definition :COLOR"

# ---- OVERLOAD IDENTITY: the semantic belongs to the resolved declaration ----
# `float[2]` and `float4[2]` are distinct overloads that agree on every FLAT
# TypeNode field; only elementType separates them.  A shallow comparison merged
# them and produced an over-refusal in one direction and a missed refusal in the
# other, so both directions are rows.
# The STRUCT pair beside it is the same argument one type category over: array
# identity hides in TypeNode::elementType, struct identity in structName and
# structFields.  CgType::equals reaches both today, so one comparator serves
# them - and a future struct-specific narrowing would pass every array row.
accept fp_c5122_overload_array_bare_f "overload: the UNCALLED one carries the semantic"
refuse fp_c5122_overload_array_sem_f  "overload: the CALLED one carries the semantic"
accept fp_c5122_overload_struct_bare_f "overload: struct, the UNCALLED one carries it"
refuse fp_c5122_overload_struct_sem_f  "overload: struct, the CALLED one carries it"

# ---- the accepts: each one the diagnostic's literal text would forbid ----
accept fp_c5122_param_only_f          "PARAMETER semantic on a called helper"
accept fp_c5122_uncalled_f            "full semantics on a function nothing calls"
accept fp_c5122_unreachable_caller_f  "called only by an entry-unreachable function"
accept fp_c5122_nosem_f               "called helper with no semantics (control)"

# ---- the default-argument EXCLUSION control ----
# A call in a default-argument expression creates no edge.  Both files are
# refused TODAY by the constant evaluator (t_d60fbc59), so a bare "we refuse
# it" proves nothing - it would look the same if the walk wrongly descended.
# The pair does: the two differ ONLY in the semantic, so identical diagnostics
# mean the semantic did not matter, which is what "no edge" means.
# EACH ROW MUST STAND ON ITS OWN BEFORE THE TWO ARE COMPARED.  Comparing two
# results that are both INVALID compares equal: a crash gives 134 on both files
# with empty first diagnostics, and a bare "the two agree" then reads PASS.
# codex measured exactly that against a wrapper returning 134 (2026-09-14).  So
# each file is first held to the same bar as every other refusal row here -
# exit 1 exactly, the named evaluator diagnostic, no container - and only then
# are the two compared.  When t_d60fbc59 lands and the fold starts working,
# this becomes exit 0 on both and the comparison moves to the recorded value;
# the point is that "both failed somehow" is never the evidence.
defarg_needle="has a default value this compiler cannot evaluate"
defarg_row() {  # <stem>
    local stem="$1" rc
    rc="$(compile "$stem")"
    [[ "$rc" -eq 1 ]] || fail "default-argument $stem: expected exit 1, got $rc (a crash or a success both read as 'equal' if this is skipped)"
    tr -d '\r' < "$work/$stem.log" | grep -qF "$defarg_needle" \
        || fail "default-argument $stem: exit 1 but not the evaluator refusal - $(tr -d '\r' < "$work/$stem.log" | grep -m1 -E 'error|nv40' | cut -c1-90)"
    [[ ! -s "$work/$stem.fpo" ]] || fail "default-argument $stem: refused but still wrote a container"
}
defarg_row fp_c5122_defarg_sem_f
defarg_row fp_c5122_defarg_nosem_f
sem_msg="$(tr -d '\r' < "$work/fp_c5122_defarg_sem_f.log"   | grep -m1 -E 'error|nv40')"
nosem_msg="$(tr -d '\r' < "$work/fp_c5122_defarg_nosem_f.log" | grep -m1 -E 'error|nv40')"
sem_msg="${sem_msg#*: }"; nosem_msg="${nosem_msg#*: }"
[[ -n "$sem_msg" ]] || fail "default-argument exclusion: empty diagnostic; an empty pair is not a match"
[[ "$sem_msg" == "$nosem_msg" ]] \
    || fail "default-argument exclusion: the semantic changed the DIAGNOSTIC
  with semantic: $sem_msg
  without:       $nosem_msg"
printf '%s' "$sem_msg" | grep -qF "$needle" \
    && fail "default-argument exclusion: the walk descended into a default argument"
echo "  exclusion ok: both rows refused by the evaluator with the same named"
echo "                diagnostic, so the semantic created no edge"

echo "non-entry-semantics: PASS"
