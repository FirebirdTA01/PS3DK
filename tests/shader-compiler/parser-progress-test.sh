#!/usr/bin/env bash
# t_472ff302: the parser must ALWAYS make progress, and an unknown type must be
# refused rather than hung on.
#
# Parser::error() records a diagnostic without consuming a token and without
# throwing, so before the fix a declaration that failed left the cursor where it
# was and the top-level loop called it again on the same token, for ever.  Any
# failure that neither advances nor throws hung the compiler; an unknown
# identifier in type position was the commonest way in.  95 shaders in the
# reference SDK sample tree never terminated.
#
# THE TIMEOUT IS THE RED.  On the unfixed compiler these inputs do not fail -
# they never return - so this test's failure mode on the parent is the timeout
# firing, and `timeout` then exits 124.  THAT IS WHY A PLAIN "non-zero exit"
# CHECK WOULD BE WORTHLESS HERE: 124 is non-zero, so the parent would PASS it.
# Each invalid input must exit with EXACTLY 1 - an ordinary compiler refusal -
# and 124 or any signal death is a FAILURE of this test.  Do not simplify these
# assertions into `if ! compiler ...; then ok; fi`.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-parser-progress-test.$$"
mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
limit="${PS3TC_PARSER_PROGRESS_TIMEOUT:-20s}"

# Returns the exit status; never lets the harness hang.
run_one() {
    local stem="$1"
    set +e
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "$limit" "$compiler" -p sce_fp_rsx \
          --emit-container "$work/$stem.fpo" "$shaders/$stem.cg" ) \
        >"$work/$stem.log" 2>&1
    local rc=$?
    set -e
    return $rc
}

must_refuse() {
    local stem="$1"
    local rc=0
    run_one "$stem" || rc=$?
    [[ $rc -ne 124 ]] || fail "$stem TIMED OUT after $limit - the parser did not terminate (t_472ff302). This is the defect, not a slow machine."
    [[ $rc -lt 128 ]] || fail "$stem died on a signal (exit $rc); a refusal must be an ordinary failure"
    [[ $rc -eq 1 ]] || fail "$stem exited $rc; an invalid type must be an ordinary refusal (exit 1)"
    [[ ! -s "$work/$stem.fpo" ]] || fail "$stem refused but still wrote a container"
    grep -qE ':[0-9]+:[0-9]+: error:' "$work/$stem.log" \
        || { tail -n 5 "$work/$stem.log" >&2; fail "$stem refused without a located diagnostic"; }
}

# The parameter form: refused, and the diagnostic NAMES the offending type.
must_refuse fp_unknown_type_param_f
grep -q "unknown type name 'bogusType'" "$work/fp_unknown_type_param_f.log" \
    || { tail -n 5 "$work/fp_unknown_type_param_f.log" >&2
         fail "the parameter form must name the type it does not know"; }

# The local form: PROGRESS ONLY.  Its diagnostics are a known cascade that
# names the wrong identifier - deliberately not asserted here, so that fixing
# the cascade does not require editing this test, and so that nobody reads this
# row as evidence the message is good.
must_refuse fp_unknown_type_local_f

# The control must still COMPILE and produce a container: the progress
# guarantee is error recovery, and recovery that changes what compiles is not
# recovery.
rc=0
run_one fp_known_sampler_control_f || rc=$?
[[ $rc -eq 0 ]] || { tail -n 10 "$work/fp_known_sampler_control_f.log" >&2
                     fail "the sampler2D control must still compile (exit $rc)"; }
[[ -s "$work/fp_known_sampler_control_f.fpo" ]] || fail "the control compiled but wrote no container"

printf 'parser-progress-test: ok (both hang shapes refuse with exit 1 and a located diagnostic; control still compiles)\n'
printf 'PASS: parser-progress-test\n'
