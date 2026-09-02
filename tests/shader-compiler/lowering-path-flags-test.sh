#!/usr/bin/env bash
# The lowering-path flag contract, for the release in which both spellings
# exist (the flip, 2026-09-02).
#
#   unflagged            the general lowering - the compiler
#   --legacy-lowering    the retired shape matcher
#   --general-lowering   accepted and IGNORED, one release, so the scripts
#                        and rig columns written before the flip keep working
#   both flags at once   REFUSED, either order
#
# The last one is the reason this file exists.  Last-wins would compile one
# lowering while the caller's own command line asks for the other, and a
# container carries no label saying which path produced it - so a rig stage
# that adds --legacy-lowering to a row already carrying --general-lowering
# would file a general container as legacy evidence and nothing downstream
# could tell.  A refusal costs one aborted stage; last-wins costs a verdict.
#
# The no-op alias is asserted on BYTES, not on exit status: an alias that
# quietly selected something else would still exit 0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-lowering-path-flags.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# A shape only the general path lowers, so "which path ran" is answerable
# from the verdict rather than from a comment (t_b8bb521f).
src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_half_cast_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

run() {   # $1 tag, then flags -> rc in $rc, log at $work/$1.log, container $work/$1.bin
    local tag="$1"; shift
    rc=0
    rm -f "$work/$tag.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$@" --emit-container "$work/$tag.bin" "$src"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    return 0
}

# 1. Unflagged is the general lowering.
run plain
[[ "$rc" -eq 0 ]] || {
    tail -n 5 "$work/plain.log" >&2
    fail "an unflagged run must be the GENERAL lowering, and this fixture
compiles there.  A refusal here means the default did not flip."
}

# 2. --legacy-lowering is the matcher, which refuses this shape.
run legacy --legacy-lowering
[[ "$rc" -ne 0 ]] || fail "--legacy-lowering compiled a shape only the general
path lowers: the flag did not select the matcher."

# 3. --general-lowering is a no-op alias: same BYTES as unflagged.
run alias --general-lowering
[[ "$rc" -eq 0 ]] || {
    tail -n 5 "$work/alias.log" >&2
    fail "--general-lowering must stay accepted for one release"
}
cmp -s "$work/plain.bin" "$work/alias.bin" || fail "--general-lowering produced
a different container from an unflagged run: it is meant to be accepted and
IGNORED, not to select anything."

# 4. Both flags, either order, refuse - and write nothing.
for order in "--general-lowering --legacy-lowering" "--legacy-lowering --general-lowering"; do
    # shellcheck disable=SC2086
    run both $order
    [[ "$rc" -ne 0 ]] || fail "'$order' compiled instead of refusing.  The two
flags name different lowerings and the container would not say which one ran."
    grep -q "refusing rather than picking one" "$work/both.log" || {
        tail -n 5 "$work/both.log" >&2
        fail "'$order' refused for some OTHER reason; the contradiction must be
named, or the caller cannot tell it from a compile failure."
    }
    [[ ! -s "$work/both.bin" ]] || fail "'$order' refused but left a container
behind, which a stage would pick up as evidence."
done

printf 'PASS: lowering-path-flags-test\n'
