#!/usr/bin/env bash
# fp_write_mask_clobber: color = float4(u*v, u*0.5+0.25, 1-u, 1) from
# uv = TEXCOORD0.xy.  The bug it was reduced from (t_daf2da77) clobbered
# neighbouring lanes: an intermediate written with a full mask overwrote a
# lane another component still needed.  The matcher-era check pinned the
# first two MUL write masks; the general path emits a different shape, so
# the program is judged by VALUE instead - fp_eval runs the emitted
# container over a grid of (u, v) and every colour lane must match.  Judged
# by value, a clobbered lane cannot pass.  A program fp_eval cannot model
# fails rather than going unjudged.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_write_mask_clobber.fcg"
work="${TMPDIR:-/tmp}/ps3dk-shader-mask-clobber-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

log="$work/fp_write_mask_clobber.log"
fpo="$work/fp_write_mask_clobber.fpo"
rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$fpo" "$src"
) >"$log" 2>&1 || rc=$?

if [[ "$rc" -eq 124 ]]; then
    fail "fp_write_mask_clobber timed out"
fi
if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
    fail "fp_write_mask_clobber aborted or was killed under memory cap"
fi
if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$log"; then
    fail "fp_write_mask_clobber reported an allocation abort"
fi
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$log" >&2
    fail "fp_write_mask_clobber failed to compile"
fi

[[ -s "$fpo" ]] || fail "fp_write_mask_clobber wrote no container"

python3 - "$repo_root/tests/shader-compiler" "$fpo" <<'PY'
import sys

sys.path.insert(0, sys.argv[1])
import fp_eval  # noqa: E402

# The evaluator's own green and red rows first: a judge that cannot refuse
# an undefined or partially written output would make every verdict below
# vacuous.
if not fp_eval.self_test():
    raise SystemExit("FAIL: fp_eval self-test failed; the value check below cannot be trusted")

blob = open(sys.argv[2], "rb").read()
grid = [-1.5, -0.75, -0.25, 0.0, 0.25, 0.5, 1.0, 1.25]
f32 = fp_eval.f32
bad = []
for u in grid:
    for v in grid:
        # Every grid value and every result is exact in binary32.
        want = [f32(u * v), f32(f32(u * 0.5) + 0.25), f32(1.0 - u), 1.0]
        try:
            got = fp_eval.evaluate(blob, {"TEX0": [u, v, 0.0, 0.0]})
        except fp_eval.Unmodelled as e:
            raise SystemExit(
                "FAIL: fp_eval cannot judge fp_write_mask_clobber (%s); an "
                "unjudged program is not a pass" % e)
        if got != want:
            lanes = [i for i in range(4) if got[i] != want[i]]
            bad.append("uv=(%g, %g): lanes %s got %s, want %s"
                       % (u, v, "".join("xyzw"[i] for i in lanes), got, want))
if bad:
    raise SystemExit(
        "FAIL: fp_write_mask_clobber colour is wrong at %d of %d grid points "
        "- a lane was clobbered or miscomputed (t_daf2da77):\n  %s"
        % (len(bad), len(grid) ** 2, "\n  ".join(bad[:8])))
PY

printf 'write-mask-clobber-test: ok\n'
