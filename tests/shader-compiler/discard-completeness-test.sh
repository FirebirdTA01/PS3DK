#!/usr/bin/env bash
# A shader whose post-discard work the default path cannot lower must be
# REFUSED, not compiled with that work missing (t_72810bd7).
#
# Two controls, because a guard like this fails in both directions: it can
# miss the drop, or it can refuse the whole discard class.  The negative
# control is a real discard shader from the samples that the default path
# lowers correctly today.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-discard-completeness-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run() {   # $1 shader path, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$2.fpo" "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    return "$rc"
}

# POSITIVE: post-discard work this path cannot lower -> must refuse.
pos="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_discard_then_work_f.cg"
if run "$pos" positive; then
    fail "fp_discard_then_work COMPILED. The post-discard lerp is not a shape
this path lowers, so the container it just produced is missing part of the
shader. Refusing is the required behaviour until the shape is lowered."
fi
if ! grep -q "t_72810bd7" "$work/positive.log"; then
    tail -n 10 "$work/positive.log" >&2
    fail "fp_discard_then_work was refused, but not by the completeness
guard - so this test would keep passing if the guard were removed and the
shader merely failed to parse."
fi

# NEGATIVE: a discard shader this path lowers correctly -> must still compile.
neg="$repo_root/samples/gcm/hello-ppu-cellgcm-discard-blend/shaders/fpshader.fcg"
if [[ -f "$neg" ]]; then
    if ! run "$neg" negative; then
        tail -n 10 "$work/negative.log" >&2
        fail "the discard-blend sample no longer compiles: the guard is
refusing the whole discard class rather than the dropped-work case"
    fi
else
    fail "negative control shader is missing: $neg"
fi

printf 'discard-completeness-test: ok\n'
