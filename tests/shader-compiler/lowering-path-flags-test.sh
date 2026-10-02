#!/usr/bin/env bash
# The lowering-path flag contract after the retired shape matcher was
# removed (chore/rsxcg-remove-legacy-lowering).
#
#   unflagged            the general lowering - the only back end
#   --general-lowering   accepted and IGNORED, so the scripts and rig columns
#                        written before the flip keep working
#   RSXCG_GENERAL=1      accepted and IGNORED, same reason
#   --legacy-lowering    REFUSED: exit 1, no container, "has been removed"
#   RSXCG_GENERAL=0      REFUSED the same way
#
# The refusals are the reason this file exists.  A caller still asking for
# the matcher must not silently get a general container filed as legacy
# evidence: a container carries no label saying which path produced it.
#
# The no-op spellings are asserted on BYTES, not on exit status: an alias
# that quietly selected something else would still exit 0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# A refusal is exit 1 EXACTLY.  124 is a timeout and >= 128 is a signal, and
# either one satisfies "did not exit 0" while meaning the compiler never
# reached the decision this guard is about - so a compiler that CRASHED on a
# shader it should have refused BY NAME was reported as correct here.  Call
# this wherever a compile's status is captured, whichever way that compile is
# expected to go: it is silent for 0 and for 1 and names anything else.
# Measured: half the guards in this suite that assert a refusal could not tell
# one from a SIGABRT (crash-versus-refusal-status).
refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-lowering-path-flags.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# Rows 1, 2 and 4 must not inherit a caller's RSXCG_GENERAL.
unset RSXCG_GENERAL

# A shape only the general path ever lowered (half-precision-lowering).
src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_half_cast_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

# $1 tag, then flags -> rc in $rc, stdout $work/$1.log, stderr $work/$1.err,
# container $work/$1.bin
run() {
    local tag="$1"; shift
    rc=0
    rm -f "$work/$tag.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$@" --emit-container "$work/$tag.bin" "$src"
    ) >"$work/$tag.log" 2>"$work/$tag.err" || rc=$?
    refusal_status "$rc" "$tag"
    return 0
}

same_as_plain() {   # $1 tag, $2 spelling
    [[ "$rc" -eq 0 ]] || {
        tail -n 5 "$work/$1.log" "$work/$1.err" >&2
        fail "$2 must stay accepted as a no-op"
    }
    cmp -s "$work/plain.bin" "$work/$1.bin" || fail "$2 produced a different
container from an unflagged run: it is meant to be accepted and IGNORED, not
to select anything."
}

removed() {   # $1 tag, $2 spelling
    [[ "$rc" -eq 1 ]] || {
        tail -n 5 "$work/$1.log" "$work/$1.err" >&2
        fail "$2 exited $rc; it must refuse with exit 1 now the shape matcher
is gone."
    }
    # the refusal is a DIAGNOSTIC, so it is read from stderr
    grep -q "has been removed" "$work/$1.err" || {
        tail -n 5 "$work/$1.err" >&2
        fail "$2 refused for some OTHER reason; the removal must be named, or
the caller cannot tell it from a compile failure."
    }
    [[ ! -e "$work/$1.bin" ]] || fail "$2 refused but left a container
behind, which a stage would pick up as evidence."
}

# 1. Unflagged is the general lowering.
run plain
[[ "$rc" -eq 0 ]] || {
    tail -n 5 "$work/plain.log" "$work/plain.err" >&2
    fail "an unflagged run must be the GENERAL lowering, and this fixture
compiles there."
}
[[ -s "$work/plain.bin" ]] || fail "an unflagged run wrote no container"

# 2. --general-lowering is a no-op alias: same BYTES as unflagged.
run alias --general-lowering
same_as_plain alias --general-lowering

# 3. RSXCG_GENERAL=1 is a no-op too.
RSXCG_GENERAL=1 run env_general
same_as_plain env_general RSXCG_GENERAL=1

# 4. --legacy-lowering asks for the removed matcher and refuses.
run legacy --legacy-lowering
removed legacy --legacy-lowering

# 5. RSXCG_GENERAL=0 asks for it through the environment and refuses.
RSXCG_GENERAL=0 run env_legacy
removed env_legacy RSXCG_GENERAL=0

printf 'PASS: lowering-path-flags-test\n'
