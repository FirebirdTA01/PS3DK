#!/usr/bin/env bash
# A CALL RESOLVES AGAINST THE DECLARATIONS VISIBLE AT THAT CALL.
# Measured against sce-cgc 475, sce_fp_rsx (t_36492ad8, commit 1 of 2).
#
# NO DEFAULT ARGUMENT APPEARS IN ANY ROW HERE.  That is deliberate: the rule
# is observable without them, which is why it lands as its own commit ahead of
# the defaults work rather than entangled with it.  Defaults only made it
# URGENT - materialising an omitted argument means asking which declaration
# carries the default, and that question has no answer until "visible at the
# call" means something.
#
# Our frontend is two-pass: pass 1 collects every declaration into the symbol
# table, pass 2 analyses bodies.  So before this commit the whole unit was
# visible from everywhere and two measured things were wrong:
#
#   FORWARD CALL    calling a helper declared later, with no prototype, was
#                   accepted; the reference refuses it (C1008).
#   LATE SHADOW     a source overload of a builtin's name written AFTER the
#                   call hid the builtin; the reference uses the builtin.
#
# WHY THE PAIRS.  Acceptance cannot say WHICH BODY RAN, and that is the whole
# question for the shadow rows.  Each pair differs only in a constant in the
# source body, so:
#
#   late pair  IDENTICAL  -> the builtin ran; the late source was never read
#   early pair DIFFER     -> the source ran
#
# A compiler that ignored source overloads of builtin names entirely would
# satisfy the late pair and fail the early one; one that always preferred the
# source would do the reverse.  Neither row alone is worth anything.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || { echo "usage: $0 <rsx-cg-compiler>" >&2; exit 2; }
[[ -x "$compiler" ]] || { echo "FAIL: not executable: $compiler" >&2; exit 1; }

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

compile() {  # <stem> -> rc
    local stem="$1"
    set +e
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" \
        "$shaders/$stem.cg" > "$work/$stem.log" 2>&1
    local rc=$?
    set -e
    printf '%s' "$rc"
}

accept() {  # <stem> <what>
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 0 ]] || fail "$2: expected exit 0, got $rc - $(tr -d '\r' < "$work/$1.log" | grep -m1 -E 'error|nv40' | cut -c1-90)"
    [[ -s "$work/$1.fpo" ]] || fail "$2: exit 0 but wrote no container"
    echo "  accept ok: $2"
}

refuse() {  # <stem> <what>
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$2: expected exit 1, got $rc"
    [[ ! -s "$work/$1.fpo" ]] || fail "$2: refused but still wrote a container"
    echo "  refuse ok: $2"
}

refuse_saying() {  # <stem> <pattern> <what>
    # For a row where the VERDICT alone is vacuous: the pristine parent refuses
    # the same program for an unrelated reason, so the diagnostic is what
    # separates the compilers.
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$3: expected exit 1, got $rc"
    # THE SAME NO-CONTAINER ASSERTION refuse() MAKES.  Omitting it let a
    # compiler exit 1 with the right words AND still write an artifact, and
    # the whole guard passed - proven with a wrapper that did exactly that
    # (review: codex).  A refusal contract is a verdict, a reason AND an
    # absence; checking two of the three is not checking the contract.
    [[ ! -s "$work/$1.fpo" ]] || fail "$3: refused but still wrote a container"
    grep -qE "$2" <(tr -d '
' < "$work/$1.log")         || fail "$3: refused, but not for the named reason - wanted /$2/, got: $(tr -d '
' < "$work/$1.log" | grep -m1 -iE 'error|nv40' | cut -c1-80)"
    echo "  refuse-saying ok: $3"
}

same() {  # <a> <b> <what>
    cmp -s "$work/$1.fpo" "$work/$2.fpo" \
        || fail "$3: $1 and $2 differ - the LATE source body reached the call"
    echo "  builtin ok: $3"
}

differs() {  # <a> <b> <what>
    cmp -s "$work/$1.fpo" "$work/$2.fpo" \
        && fail "$3: $1 and $2 are IDENTICAL - the EARLY source body never ran"
    echo "  source ok: $3"
}

# ---- a declaration written after the call is not a candidate ----
refuse fp_visibility_forward_refuse_f \
       "a forward call with no prior prototype (reference: C1008)"
accept fp_visibility_prototype_f \
       "the same call WITH a prior prototype"
accept fp_visibility_defined_before_f \
       "defined before the call"

# ---- the source/builtin partition is positional too ----
accept fp_visibility_late_source_050_f  "late source overload of a builtin name (0.5)"
accept fp_visibility_late_source_075_f  "late source overload of a builtin name (0.75)"
same   fp_visibility_late_source_050_f fp_visibility_late_source_075_f \
       "a source overload declared AFTER the call does not hide the builtin"

accept fp_visibility_early_source_050_f "early source overload of a builtin name (0.5)"
accept fp_visibility_early_source_075_f "early source overload of a builtin name (0.75)"
differs fp_visibility_early_source_050_f fp_visibility_early_source_075_f \
       "a source overload declared BEFORE the call DOES hide the builtin"

# ---- walk parity between the two passes (review: Fable) ----
# Every top-level declaration KIND in one unit.  This row CANNOT fail today -
# both passes are the same `for` over unit.declarations with the counter bumped
# before the switch, so parity holds by construction - and that is exactly why
# it is here: the day a filter or a `continue` appears in one loop and not the
# other, this stops compiling and the compiler names the parity error instead
# of shifting every index after the divergence in silence.
accept fp_visibility_walk_parity_f        "struct + uniform + const + prototype + helper + entry in one unit"

# ---- the C1008 name-not-found class is gated on entry reachability ----
# Found by codex reviewing the first cut of this commit: the positional filter
# refused a forward call inside a function main never calls, and the reference
# does not.  The reference reports this class ONLY in reachable bodies - but it
# reports type and arity errors everywhere, so "unreachable bodies are not
# analysed" is the wrong reading and the REFUSE rows below are what rule it out.
accept fp_reach_unreached_forward_f        "forward call in an unreachable body"
accept fp_reach_unreached_undecl_id_f        "undeclared identifier in an unreachable body (pre-existing)"
accept fp_reach_unreached_undecl_fn_f        "undeclared function in an unreachable body (pre-existing)"
refuse fp_reach_unreached_type_refuse_f        "a TYPE error is not gated - C1056 fires unreachable"
refuse fp_reach_unreached_arity_refuse_f        "a wrong ARITY is not gated - C1103 fires unreachable"
refuse fp_reach_visible_name_arity_refuse_f        "a VISIBLE name with wrong arity is C1103 even with a later exact overload"
accept fp_reach_suppressed_no_cascade_f        "a suppressed missing name does not cascade into an arity error"

# The reachable mirrors.  Each ACCEPT above is satisfied by a compiler that
# never reports the class at all; these are what stop that.
refuse fp_reach_reachable_forward_refuse_f        "the same forward call in a REACHABLE body"
refuse fp_reach_reachable_undecl_fn_refuse_f        "undeclared function in a REACHABLE body"
refuse fp_reach_reachable_undecl_id_refuse_f        "undeclared identifier in a REACHABLE body"
refuse fp_reach_if_false_refuse_f        "no branch pruning: a forward call inside if(false) in the entry"

# ---- a name that EXISTS but is not a function is C1105, never deferred ----
refuse fp_reach_callee_local_var_refuse_f        "a local variable as the callee, in an unreachable body"
refuse fp_reach_callee_global_var_refuse_f        "a file-scope variable as the callee, in an unreachable body"

# ---- a file-scope initialiser is a reachability root ----
refuse fp_reach_init_root_refuse_f        "a static initialiser reaches its callee's body"
refuse fp_reach_init_relay_refuse_f        "the same through a relay - the root is transitive"
accept fp_reach_init_root_valid_f        "the same shape with a valid body (control)"

# C5122 shares the widened root set.  BY DIAGNOSTIC: the pristine parent also
# refuses this program, but on "ldunif of 'g' has no registered uniform source"
# - the file-scope-initialiser lowering gap - so asserting the verdict alone
# would pass on a compiler that has never heard of C5122.
refuse_saying fp_reach_c5122_via_init_refuse_f        "semantics not allowed on functions other than the entry"        "a return semantic on a helper reached only from a static initialiser"

# ---- a nearer binding wins over the function of the same name ----
refuse fp_reach_local_shadows_fn_refuse_f        "a local shadows a visible function, unreachable body"
refuse fp_reach_local_shadows_fn_reachable_refuse_f        "the same shadow in a REACHABLE body"
accept fp_reach_no_shadow_calls_fn_f        "no shadow: the same call reaches the function (control)"

# ---- a uniform default initialiser roots the walk too ----
refuse fp_reach_uniform_default_root_refuse_f        "a uniform default initialiser reaches its callee's body"

# ---- used here, declared later HERE: C1002, and not reachability-gated ----
# The deferral introduced by this commit newly ACCEPTED the first row; the
# reference refuses it.  Keyed by the SCOPE INSTANCE - the three ACCEPT rows
# below are what rule out keying it by function (over-refuses all three) or by
# scope depth (cannot tell the two siblings apart).
refuse fp_scope_late_local_same_refuse_f        "used then declared in the same block, unreachable"
refuse fp_scope_late_local_same_reachable_refuse_f        "the same, REACHABLE - the refusal is unconditional"
refuse fp_scope_late_local_value_refuse_f        "the use is a VALUE read, not a call"
accept fp_scope_late_local_inner_f        "use OUTER, declare INNER (legal)"
accept fp_scope_late_local_outer_f        "use INNER, declare OUTER (legal)"
accept fp_scope_late_local_siblings_f        "two SIBLING blocks (legal; rules out keying on depth)"

# ---- recording a name is not diagnosing it ----
# An error-typed argument suppresses the diagnostics on its call, but must not
# suppress the RECORD that the callee's name was used unresolved.  The two
# ACCEPT rows are what keep the fix from becoming "emit C1105 earlier", which
# would refuse two programs the reference compiles (review: codex).
refuse fp_scope_nested_unresolved_callee_refuse_f        "unresolved callee with an error-typed argument, declared later"
refuse fp_scope_nested_unresolved_bare_refuse_f        "the same with a bare error-typed argument"
accept fp_scope_nested_no_later_decl_f        "the same unresolved callee with NO later declaration"
accept fp_scope_nested_shadow_suppressed_f        "a shadowing local called with an error-typed argument (suppressed)"

# ---- the bookkeeping asks about a VISIBLE binding, not lookup success ----
refuse fp_scope_later_fn_collides_refuse_f        "callee resolved only by a LATER function, local declared after"
refuse fp_scope_later_fn_nested_refuse_f        "the same through a nested-call argument"
accept fp_scope_earlier_fn_shadow_f        "the same body with the function declared BEFORE (legal shadow)"
accept fp_scope_later_fn_no_local_f        "recorded but never collided (no local declaration)"

# SELF-CHECK: the guard's own refusal contract.  refuse_saying() shipped for
# ten minutes checking the verdict and the diagnostic but NOT the absence of a
# container, and a compiler that exited 1 with the right words while writing an
# artifact passed the whole suite (review: codex).  This re-runs this script
# against a stub that does exactly that and requires it to FAIL.  A guard
# without a self-test is a guard nobody has tested.
#
# IT RUNS LAST, ON PURPOSE.  The stub delegates every other row to the real
# compiler, so on a RED compiler it would never reach the row it leaks on and
# would report a confusing failure about the wrong assertion.  Reaching here
# means every row above passed, so the stub's run differs from this one in
# exactly one place.
if [[ -z "${CALL_VISIBILITY_SELFTEST:-}" ]]; then
    leak="$work/leaky-compiler.sh"
    cat > "$leak" <<LEAK
#!/usr/bin/env bash
out=""; prev=""
for x in "\$@"; do [[ "\$prev" == "--emit-container" ]] && out="\$x"; prev="\$x"; done
case " \$* " in
  *fp_reach_c5122_via_init_refuse_f.cg*)
    printf 'x' > "\$out"
    echo "error: semantics not allowed on functions other than the entry function: 'helper'" >&2
    exit 1;;
esac
exec "$compiler" "\$@"
LEAK
    chmod +x "$leak"
    if CALL_VISIBILITY_SELFTEST=1 bash "${BASH_SOURCE[0]}" "$leak" > "$work/selftest.log" 2>&1; then
        fail "SELF-CHECK: a compiler that refuses with the right words but still writes a container PASSED this guard"
    fi
    grep -q "still wrote a container" "$work/selftest.log"         || fail "SELF-CHECK: the leaky stub failed, but not on the container assertion - $(grep -m1 FAIL "$work/selftest.log" | cut -c1-80)"
    echo "  self-check ok: the refusal contract catches a leaked container"
fi

echo "call-visibility: PASS"
