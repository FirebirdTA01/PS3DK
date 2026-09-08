#!/usr/bin/env bash
# Every lane of the colour output carries the VALUE the fixture computes
# (t_856689b2).
#
# The rule, its scope, and why the two earlier versions of this guard were
# too weak - one asserted a shape that 06baa4eb legitimately changed, the
# other asserted lane indices, which codex bypassed in review by pointing
# the final MOV at the original input - are in insert_lane_check.py.
#
# THE CHECK IS RUN EIGHT TIMES.  Once on what the compiler emitted, which
# must pass, and once on each of seven DOCTORED copies of those same words,
# each of which must be refused with status EXACTLY 1:
#
#   broadcast   the original defect: the source swizzle forced to x
#   bypass      codex's first finding: the same lanes read straight from
#               the input, so the multiply never happens
#   constant    one factor in the inline const block doubled
#   hwinput     codex's second finding: the multiply's per-instruction
#               input selector moved to another varying, so RGB and w come
#               from different attributes
#   earlyend    codex's third finding: PROGRAM_END set on the first
#               instruction, so the hardware stops before the multiply and
#               before the final colour write
#   saturate    the multiply's destination saturation bit set
#   halfdst     the multiply's destination moved to the half bank while its
#               reader still names R
#
# A guard nobody has seen fail is not a guard, and "nonzero" is not the
# same as "refused" - a crash would satisfy -ne 0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_insert_lanes_of_vector_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

check="$repo_root/tests/shader-compiler/insert_lane_check.py"
mutate="$repo_root/tests/shader-compiler/insert_lane_mutate.py"
for helper in "$check" "$mutate"; do
    [[ -f "$helper" ]] || fail "helper missing: $helper"
done

python="${PYTHON:-python3}"
command -v "$python" >/dev/null 2>&1 || fail "no python3: this guard decodes \
the emitted ucode and cannot fall back to a text match, so it fails rather \
than reporting a green it did not earn"

work="${TMPDIR:-/tmp}/ps3dk-insert-lane-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# STDOUT AND STDERR SEPARATELY.  A merged capture lets a diagnostic land
# inside a hex row; the decoder then either raises or, before it learned
# to, silently dropped the row and misaligned every row after it.
rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$work/general.log" 2>"$work/general.err" || rc=$?
[[ "$rc" -eq 124 ]] && fail "fp_insert_lanes_of_vector timed out"
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/general.err" >&2
    fail "fp_insert_lanes_of_vector did not compile on the general path"
fi

"$python" "$check" "$work/general.log" \
    || fail "the emitted program does not compute the fixture's values"

for kind in broadcast bypass constant hwinput earlyend saturate halfdst; do
    "$python" "$mutate" "$work/general.log" "$work/$kind.log" "$kind" \
        || fail "the $kind red control could not be built from the emitted \
words, so it would prove nothing"
    set +e
    "$python" "$check" "$work/$kind.log" >"$work/$kind.out" 2>&1
    control_rc=$?
    set -e
    if [[ "$control_rc" -ne 1 ]]; then
        cat "$work/$kind.out" >&2
        fail "the $kind red control returned $control_rc, expected exactly 1 \
- a doctored program must be REFUSED, and a pass or a crash here means the \
check above is not testing what it claims"
    fi
done

printf 'insert-lane-of-vector-test: ok (all seven red controls refused)\n'
