#!/usr/bin/env bash
# A DEFAULT ON A HELPER PARAMETER IS THE EXPRESSION, SUBSTITUTED AT THE CALL
# SITE.  Measured against sce-cgc 475, sce_fp_rsx (t_36492ad8).
#
# This is A1's contract INVERTED, which is why the two slices needed separate
# guards.  An entry parameter's default is RECORDED - a defaultValue block, an
# embedded-constant offset, a reflection record you can read.  A helper
# parameter's default is not recorded at all: the container has no record for
# it, because the argument is materialised where the call is written.  An
# assertion looking for a defaultValue record for a helper parameter would be
# looking for something the reference never emits.
#
# WHAT IS ASSERTED, and why each row cannot be dropped:
#
#   TWIN        shade(t) must byte-equal shade(t, 0.5) written out.  That is
#               the whole correctness statement and byte-identity is available,
#               so nothing weaker is used.
#   NEAR MISS   the same shape with a different default must MOVE the
#               container.  Without it, a compiler that dropped the omitted
#               argument entirely would satisfy the twin for the wrong reason -
#               both spellings would be equally wrong and equally equal.
#   UNIFORM     a default that names a uniform must byte-equal passing that
#               uniform explicitly.  THIS IS THE ONLY ROW THAT SEPARATES
#               SUBSTITUTION FROM FOLDING; every literal row passes under both
#               implementations, and folding is the natural thing to write.
#   PREFER      the DEFAULTED float4 overload is chosen over f(float3), NOT
#               f(float3).  Asserted by comparing against a program that
#               declares only the chosen overload, because acceptance alone
#               cannot say which body ran.
#
#               READ THIS ROW'S STRENGTH HONESTLY.  On the REFERENCE this is a
#               genuine preference: it accepts a float4 argument for a float3
#               parameter with "warning C7011: implicit cast", so both
#               candidates are viable there and it ranks them.  WE HAVE NO SUCH
#               CONVERSION - f(float3) is not viable at all for a float4
#               argument - so for us the row passes by ELIMINATION and would
#               pass under any ranking rule whatsoever.  It is pinned anyway
#               because it pins WHICH BODY RAN, and because the day t_3aa92146
#               lands it becomes the preference test it reads like.
#               fp_helper_default_narrow_gap_f is the row that makes that
#               dependency explicit and goes red at the same moment.
#   REFUSALS    a default naming another parameter, and an exact-arity overload
#               beside a defaulted one.  Both by VERDICT: the reference reports
#               the first as C1102 "incompatible type for parameter #3", which
#               describes a scope failure as a type error, and pinning that
#               text would encode its confusion rather than its rule.
#
# ---------------------------------------------------------------------------
# COMMIT 2 adds: a default on EVERY parameter (not only an entry uniform's),
# the reachability rule that decides WHEN a default is analysed, and the
# out/inout argument rule.
#
# THE out/inout RULE IS NOT AN ARITY RULE, and the four ambiguity rows are what
# prove it.  A defaulted out/inout parameter still RANKS: beside a plain
# overload of the supplied arity the call ties and the reference says
#
#     error C1101: ambiguous overloaded function reference "f"
#
# in both declaration orders - identically to a plain `float k=.5` parameter.
# Only the SINGLE-candidate case is refused on the argument:
#
#     error C1111: non-lvalue actual parameter #2 cannot be out parameter ("o")
#
# An interim build that excluded the defaulted out/inout candidate from
# viability ACCEPTED all four ambiguity programs.  That is the class we never
# ship, and it is why the exclusion was retracted and the check moved after
# ranking.  The mutant for these rows is that exclusion restored.
#
# THE THREE out/inout ACCEPT CELLS ARE DELIBERATELY ABSENT.  The reference
# accepts `f(t, k)` where k is a declared lvalue, in both out and inout
# spellings, and accepts the declaration alone with no call.  We refuse the
# first two in the IR inliner ("out/inout parameters are not supported"), which
# is t_b11c19df, not this slice.  Pinning them here would pin the WRONG
# verdict.  The six reference verdicts are recorded on t_b11c19df as its
# acceptance criteria; when it lands, those three rows belong in this file.
#
# WHY THE REFUSAL ROWS PIN THE DIAGNOSTIC AND NOT JUST exit 1.  A call that
# omits a defaulted out parameter can reach two different refusals: resolution
# failing in semantic, or resolution succeeding and the inliner refusing what
# it materialised.  Both exit 1; only one is the reference's reason.  The
# control fp_helper_default_outnodef_refuse_f - same omission, no default -
# is the reference's C1103 and must NOT carry the C1111 text, which is what
# keeps the pair from degenerating into "both refuse".
#
# THE AMBIGUITY ROW BELONGS TO THIS SLICE even though it is about overload
# resolution: before defaults were materialised, a two-parameter overload was
# never viable for a one-argument call, so `f(float4)` beside
# `f(float4, float k = 1.0)` could not be ambiguous.  Widening the arity filter
# is what makes it reachable, and a resolver that preferred exact arity would
# silently pick one where the reference refuses.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || { echo "usage: $0 <rsx-cg-compiler>" >&2; exit 2; }
[[ -x "$compiler" ]] || { echo "FAIL: not executable: $compiler" >&2; exit 1; }

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

compile() {  # <stem> -> rc; container at $work/<stem>.fpo, log at .log
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
}

refuse() {  # <stem> <what>
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$2: expected exit 1, got $rc"
    [[ ! -e "$work/$1.fpo" ]] || fail "$2: refused but still wrote a container"
    echo "  refuse ok: $2"
}

refuse_saying() {  # <stem> <pattern> <what>
    # FOR A ROW WHERE THE VERDICT ALONE IS VACUOUS.  Several programs here
    # refuse for more than one possible reason - resolution failing, the
    # inliner refusing what resolution let through, a gap in an unrelated
    # feature - so exit 1 identifies nothing.  The contract is a verdict, a
    # reason AND an absence; checking two of the three is not checking it.
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$3: expected exit 1, got $rc"
    [[ ! -e "$work/$1.fpo" ]] || fail "$3: refused but still wrote a container"
    grep -qE "$2" <(tr -d '\r' < "$work/$1.log") \
        || fail "$3: refused, but not for the named reason - wanted /$2/, got: $(tr -d '\r' < "$work/$1.log" | grep -m1 -iE 'error|nv40' | cut -c1-80)"
    echo "  refuse-saying ok: $3"
}

refuse_not_saying() {  # <stem> <pattern> <what>
    # THE COMPLEMENT, and the reason the C1111 rows are not vacuous.  This
    # program must refuse for a DIFFERENT reason than the named one.  Without
    # it a compiler that emitted the C1111 text for every failed call would
    # satisfy every refuse_saying row above, and the rule they pin - that an
    # omitted out argument is not the same failure as too few arguments -
    # would be unasserted.
    local rc; rc="$(compile "$1")"
    [[ "$rc" -eq 1 ]] || fail "$3: expected exit 1, got $rc"
    [[ ! -e "$work/$1.fpo" ]] || fail "$3: refused but still wrote a container"
    if grep -qE "$2" <(tr -d '\r' < "$work/$1.log"); then
        fail "$3: refused with /$2/, which is the OTHER rule's diagnostic"
    fi
    echo "  refuse-not-saying ok: $3"
}

compile_vp() {  # <stem> -> rc; container at $work/<stem>.fpo, log at .log
    local stem="$1"
    set +e
    "$compiler" -p sce_vp_rsx --emit-container "$work/$stem.fpo" \
        "$shaders/$stem.cg" > "$work/$stem.log" 2>&1
    local rc=$?
    set -e
    printf '%s' "$rc"
}

accept_vp() {  # <stem> <what>
    local rc; rc="$(compile_vp "$1")"
    [[ "$rc" -eq 0 ]] || fail "$2: expected exit 0, got $rc - $(tr -d '\r' < "$work/$1.log" | grep -m1 -E 'error|nv40' | cut -c1-90)"
    [[ -s "$work/$1.fpo" ]] || fail "$2: exit 0 but wrote no container"
}

same() {  # <a> <b> <what>
    cmp -s "$work/$1.fpo" "$work/$2.fpo" \
        || fail "$3: $1 and $2 differ ($(stat -c%s "$work/$1.fpo") vs $(stat -c%s "$work/$2.fpo") bytes)"
    echo "  twin ok: $3"
}

differs() {  # <a> <b> <what>
    cmp -s "$work/$1.fpo" "$work/$2.fpo" \
        && fail "$3: $1 and $2 are IDENTICAL - the default never reached the container"
    echo "  near-miss ok: $3"
}

# ---- the omitted argument is the default, and it equals writing it out ----
accept fp_helper_default_f            "helper default omitted at the call site"
accept fp_helper_default_twin_f       "the same argument written out"
accept fp_helper_default_near_f       "the same shape with a different default"
same    fp_helper_default_f fp_helper_default_twin_f \
        "omitted default == explicit argument"
differs fp_helper_default_f fp_helper_default_near_f \
        "a changed default moves the container"

# ---- the row that separates substitution from folding ----
accept fp_helper_default_uniform_f      "default naming a uniform"
accept fp_helper_default_uniform_twin_f "that uniform passed explicitly"
same    fp_helper_default_uniform_f fp_helper_default_uniform_twin_f \
        "a uniform default substitutes the EXPRESSION, not a folded constant"

# ---- shapes the reference accepts that this slice must not refuse ----
accept fp_helper_default_all_f      "defaults on every parameter, called with none"
accept fp_helper_default_nested_f   "a defaulted helper called from a defaulted helper"
accept fp_helper_default_midparam_f "a default on a NON-trailing parameter"

# ---- overload resolution, which widening the arity filter changed ----
accept fp_helper_default_prefer_f      "exact match with a default fill vs a conversion"
accept fp_helper_default_prefer_twin_f "only the overload the reference chooses"
same    fp_helper_default_prefer_f fp_helper_default_prefer_twin_f \
        "the DEFAULTED float4 overload is chosen, not f(float3) (by elimination for us - see the header and t_3aa92146)"

refuse fp_helper_default_crossparam_refuse_f "a default naming another parameter"
refuse fp_helper_default_ambiguous_refuse_f  "exact arity beside a defaulted overload"

# ---- an omitted out/inout argument: refused on the ARGUMENT, after ranking ----
refuse_saying fp_helper_default_out_omit_refuse_f \
    "C1111" "omitting a defaulted out parameter"
refuse_saying fp_helper_default_inout_omit_refuse_f \
    "C1111" "omitting a defaulted inout parameter"
refuse_not_saying fp_helper_default_outnodef_refuse_f \
    "C1111" "omitting an out parameter with NO default is the other rule (C1103)"

refuse_saying fp_helper_default_out_amb_deffirst_refuse_f \
    "ambiguous" "defaulted out beside a plain overload, default declared first"
refuse_saying fp_helper_default_out_amb_plainfirst_refuse_f \
    "ambiguous" "the same pair in the reversed declaration order"
refuse_saying fp_helper_default_inout_amb_deffirst_refuse_f \
    "ambiguous" "defaulted inout beside a plain overload, default declared first"
refuse_saying fp_helper_default_inout_amb_plainfirst_refuse_f \
    "ambiguous" "the same inout pair in the reversed declaration order"

# ---- overload ranking: narrowing is disqualifying, not merely expensive ----
accept fp_overload_widen_f      "clamp(float, int, int) - the SDK shape"
accept fp_overload_widen_twin_f "the float overload spelled out"
accept fp_overload_widen_near_f "the int overload forced"
same    fp_overload_widen_f fp_overload_widen_twin_f \
        "two widenings beat one narrowing"
differs fp_overload_widen_f fp_overload_widen_near_f \
        "the int overload really is a different container"

accept fp_overload_widen_rev_f      "clamp(int, float, float) - one widening vs two narrowings"
accept fp_overload_widen_rev_twin_f "the float overload spelled out"
accept fp_overload_widen_rev_near_f "the int overload forced"
same    fp_overload_widen_rev_f fp_overload_widen_rev_twin_f \
        "the all-widening candidate wins even needing FEWER conversions - no cost exchange rate reproduces this"
differs fp_overload_widen_rev_f fp_overload_widen_rev_near_f \
        "clamp(0,0,1) is 0, clamp(0,0.5,1.5) is 0.5 - the near miss moves the value"

accept fp_overload_widen_half_f      "clamp(float, half, half)"
accept fp_overload_widen_half_twin_f "the float overload spelled out"
same    fp_overload_widen_half_f fp_overload_widen_half_twin_f \
        "float->half is the narrowing, so float wins again"

# ---- WHEN a default is analysed: reachability, not use ----
accept fp_helper_default_unreached_f \
    "a bad default in a helper the entry never reaches"
refuse_saying fp_helper_default_reached_supplied_refuse_f \
    "nosuch" "reached helper, argument supplied, default never used"
refuse_saying fp_helper_default_reached_used_refuse_f \
    "nosuch" "reached helper, default used"

# ---- shapes the reference accepts and this slice must keep accepting ----
accept fp_helper_default_call_f      "a call in a helper default"
accept fp_helper_default_call_twin_f "that call written out"
same    fp_helper_default_call_f fp_helper_default_call_twin_f \
        "a call in a default is substituted, not folded away"

accept fp_helper_default_multisite_f      "two call sites, one omitting and one supplying"
accept fp_helper_default_multisite_twin_f "both sites written out"
same    fp_helper_default_multisite_f fp_helper_default_multisite_twin_f \
        "substitution is per call site"

accept fp_helper_default_mixed_f      "one argument supplied, one filled"
accept fp_helper_default_mixed_twin_f "both written out"
same    fp_helper_default_mixed_f fp_helper_default_mixed_twin_f \
        "only the omitted trailing argument is filled"

accept fp_helper_default_matrix_f      "a matrix-typed default"
accept fp_helper_default_matrix_twin_f "that matrix written out"
same    fp_helper_default_matrix_f fp_helper_default_matrix_twin_f \
        "a matrix default substitutes like any other expression"

accept fp_helper_default_constglob_f      "a default naming a const file-scope global"
accept fp_helper_default_constglob_twin_f "that global passed explicitly"
same    fp_helper_default_constglob_f fp_helper_default_constglob_twin_f \
        "a const global default resolves in the DECLARATION's scope"

# ---- the same rule in the vertex profile ----
accept_vp vp_helper_default_v      "helper default omitted, vertex profile"
accept_vp vp_helper_default_twin_v "the same argument written out"
accept_vp vp_helper_default_near_v "the same shape with a different default"
same    vp_helper_default_v vp_helper_default_twin_v \
        "VP: omitted default == explicit argument"
differs vp_helper_default_v vp_helper_default_near_v \
        "VP: a changed default moves the container"

# ---- a file-scope name collision with the helper ----
refuse_saying fp_helper_default_filescope_c1002_refuse_f \
    "already defined|redefinition" "a file-scope variable reusing the helper's name"

# ---- a materialised default reaches what it calls (C5122) ----
refuse_saying fp_helper_default_semcall_used_refuse_f \
    "semantics not allowed" "a default calls a helper carrying a return semantic"
refuse_saying fp_helper_default_semcall_relay_refuse_f \
    "semantics not allowed" "transitively, through a helper that has no semantic itself"
accept fp_helper_default_semcall_supplied_f \
    "THE CONTROL: the same default, argument SUPPLIED, so nothing is reached"
accept fp_helper_default_semcall_nosem_f \
    "the same shape with no semantic anywhere"

# ---- a default's names bind at the helper's DECLARATION (C1002) ----
refuse_saying fp_helper_default_selfcall_refuse_f \
    "C1002" "a default calling the function it declares"
accept fp_helper_default_mutualcall_f \
    "THE CONTROL: two defaults naming each other terminate and are accepted"

refuse_saying fp_helper_default_mutualomit_refuse_f \
    "no matching function" "two defaults naming each other with NO argument - arity stops it, not the name rule"
refuse_saying fp_helper_default_latecallee_refuse_f \
    "C1002" "a default calling a helper declared AFTER it, never reached"

# ---- a default's writes to a global survive the call ----
accept fp_helper_default_globalwrite_f      "a default whose callee writes a global the caller reads"
accept fp_helper_default_globalwrite_twin_f "that callee passed explicitly"
same    fp_helper_default_globalwrite_f fp_helper_default_globalwrite_twin_f \
        "restoring the caller scope must not undo what the default WROTE"

refuse_saying fp_helper_default_latename_refuse_f \
    "C1002" "a default naming a global declared AFTER the helper"
refuse_saying fp_helper_default_latename_unused_refuse_f \
    "C1002" "the same, with the default never used - it is a binding rule"
accept fp_helper_default_earlyname_f \
    "THE CONTROL: the same global declared BEFORE the helper"

# ---- two definitions of one signature is C1106 ----
refuse_saying fp_helper_default_redef_refuse_f \
    "C1106" "a second definition, after the call"
refuse_saying fp_helper_default_redef_nodefault_refuse_f \
    "C1106" "a second definition carrying no default"
refuse_saying fp_helper_default_redef_nocall_refuse_f \
    "C1106" "a second definition with no call at all"
refuse_not_saying fp_helper_default_proto_then_def_refuse_f \
    "C1106" "THE CONTROL: a prototype and its definition are not two definitions"

# ---- a prototype's default is the one that gets substituted ----
accept fp_helper_default_proto_f      "a default on a PROTOTYPE, definition below the call"
accept fp_helper_default_proto_twin_f "that call written out"
same    fp_helper_default_proto_f fp_helper_default_proto_twin_f \
        "a prototype default is resolved and substituted, not left dangling"

# ---- GAPS.  Each pins a refusal the reference does NOT make, by diagnostic,
# ---- so the row goes red the day its card lands and forces a rewrite here.
refuse_saying fp_helper_default_braced_struct_gap_f \
    "too much data|Complex constructor|constructor requires" \
    "t_084fba44: braced struct default (reference ACCEPTS)"
refuse_saying fp_helper_default_braced_array_gap_f \
    "constructor requires|too much data|Complex constructor" \
    "t_084fba44: braced array default (reference ACCEPTS)"
refuse_saying fp_helper_default_entrycall_gap_f \
    "cannot evaluate" \
    "t_d60fbc59: a call in an ENTRY uniform default (reference ACCEPTS)"
refuse_saying fp_helper_default_narrow_gap_f \
    "no matching function" \
    "t_3aa92146: a float4 argument for a float3 parameter (reference warns C7011 and ACCEPTS)"

# SELF-CHECK: this guard's own refusal contract.  Sixteen rows above are
# refuse_saying / refuse_not_saying, and the failure they are most exposed to
# is the one that already shipped once on the sister guard: a compiler that
# exits 1 with the right words while STILL writing a container satisfies the
# verdict and the reason and leaks the artifact (review: codex).  This re-runs
# this script against a stub that does exactly that on one row and requires the
# run to FAIL, on that assertion.  A guard with no self-test is a guard nobody
# has tested.
#
# IT RUNS LAST.  The stub delegates every other row to the real compiler, so
# against a red compiler it would stop somewhere earlier and report a confusing
# failure about an unrelated assertion.  Reaching here means every row above
# passed, so the stub's run differs from this one in exactly one place.
if [[ -z "${HELPER_DEFAULTS_SELFTEST:-}" ]]; then
    leak="$work/leaky-compiler.sh"
    cat > "$leak" <<LEAK
#!/usr/bin/env bash
out=""; prev=""
for x in "\$@"; do [[ "\$prev" == "--emit-container" ]] && out="\$x"; prev="\$x"; done
case " \$* " in
  *fp_helper_default_out_omit_refuse_f.cg*)
    printf 'x' > "\$out"
    echo 'error: error C1111: non-lvalue actual parameter #2 cannot be out parameter' >&2
    exit 1;;
esac
exec "$compiler" "\$@"
LEAK
    chmod +x "$leak"
    if HELPER_DEFAULTS_SELFTEST=1 bash "${BASH_SOURCE[0]}" "$leak" > "$work/selftest.log" 2>&1; then
        fail "SELF-CHECK: a compiler that refuses with the right words but still writes a container PASSED this guard"
    fi
    grep -q "still wrote a container" "$work/selftest.log" \
        || fail "SELF-CHECK: the leaky stub failed, but not on the container assertion - $(grep -m1 FAIL "$work/selftest.log" | cut -c1-80)"
    echo "  self-check ok: the refusal contract catches a leaked container"

    # AND AN EMPTY FILE IS A FILE.  The refusal helpers used `! -s`, which is
    # false for a zero-byte output - so a compiler that refused correctly but
    # left an empty artifact behind passed every refusal row (review: codex,
    # who demonstrated it with a wrapper rather than by reading the test).
    # They use `! -e` now, and this proves the difference: the stub below
    # writes NOTHING to a file it creates, which the old spelling accepted.
    empty="$work/empty-compiler.sh"
    cat > "$empty" <<EMPTY
#!/usr/bin/env bash
out=""; prev=""
for x in "\$@"; do [[ "\$prev" == "--emit-container" ]] && out="\$x"; prev="\$x"; done
case " \$* " in
  *fp_helper_default_out_omit_refuse_f.cg*)
    : > "\$out"
    echo 'error: error C1111: non-lvalue actual parameter #2 cannot be out parameter' >&2
    exit 1;;
esac
exec "$compiler" "\$@"
EMPTY
    chmod +x "$empty"
    if HELPER_DEFAULTS_SELFTEST=1 bash "${BASH_SOURCE[0]}" "$empty" > "$work/selftest-empty.log" 2>&1; then
        fail "SELF-CHECK: a compiler that refuses correctly but leaves an EMPTY artifact PASSED this guard"
    fi
    grep -q "still wrote a container" "$work/selftest-empty.log" \
        || fail "SELF-CHECK: the empty-output stub failed, but not on the container assertion - $(grep -m1 FAIL "$work/selftest-empty.log" | cut -c1-80)"
    echo "  self-check ok: an EMPTY artifact is a leaked container too"
fi

echo "helper-default-args: PASS"
