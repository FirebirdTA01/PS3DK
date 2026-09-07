#!/usr/bin/env bash
# t_652d6e42: output-pinned SelPred must not allocate its destination in
# the same R slot as the condition or then-value source it reads later.
#
# SelPred expands as:
#   MOV dst, else
#   MOVC CC.x, cond
#   MOV dst(NE.x), then
#
# That is not read-then-write internally.  If dst aliases src0 or src1,
# the first MOV clobbers a value the later expanded instructions still
# need, producing an always-else select.  The fixture is a tracked copy of
# the test_76 shape that exposed this after control-flow lowering started
# composing COLOR0 lane-by-lane into output-pinned R0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

# Prove the trace parser on safe adjacent records and genuine collisions.
python3 "$repo_root/tests/shader-compiler/selpred-alias-parser-test.py"

work="${TMPDIR:-/tmp}/ps3dk-selpred-output-alias-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

for stem in fp_selpred_output_alias_f fp_selpred_reuse_alias_f \
            fp_selpred_shared_scalar_f fp_selpred_scalar_assignment_f; do
src="$repo_root/tools/rsx-cg-compiler/tests/shaders/$stem.cg"
out="$work/$stem.fpo"
log="$work/$stem.log"
[[ -f "$src" ]] || fail "fixture missing: $src"

(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" env RSX_DUMP_ORDER=1 \
        "$compiler" -p sce_fp_rsx --emit-container "$out" "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "$stem.cg did not compile"
}

[[ -s "$out" ]] || fail "compiler exited 0 but produced no container"

# The plain-assignment control has an input then arm and uniform condition,
# so it has no early-read temp for the alias checker to exercise.
if [[ "$stem" != fp_selpred_scalar_assignment_f ]]; then
    python3 "$repo_root/tests/shader-compiler/selpred_alias_check.py" "$log"
fi
if [[ "$stem" == fp_selpred_reuse_alias_f ]]; then
    # Allocation alone can pass while VecInsert has already overwritten the
    # else value. This fixture computes different values in its two arms.
    python3 "$repo_root/tests/shader-compiler/selpred_alias_check.py" "$log" --distinct-arms
fi
if [[ "$stem" == fp_selpred_shared_scalar_f || "$stem" == fp_selpred_scalar_assignment_f ]]; then
    python3 - "$out" "$repo_root/tests/shader-compiler" <<'PY'
import pathlib, sys
sys.path.insert(0, sys.argv[2])
from fp_sources import instructions, ucode_words
rows = list(instructions(ucode_words(pathlib.Path(sys.argv[1]).read_bytes())))
if not rows:
    raise SystemExit("FAIL: scalar fixture has no instructions")
for words, _ in rows:
    if ((words[0] >> 24) & 0x3f) == 1 and ((words[0] >> 9) & 0xf) == 0:
        raise SystemExit("FAIL: scalar shared-base insert emitted an empty-mask MOV")
PY
fi
done

# A base copy must preserve only the declared lanes. Writing an absent z/w
# lane here can be folded into the vertex export and change its defaults.
src="$repo_root/tools/rsx-cg-compiler/tests/shaders/vp_shared_insert_narrow_v.cg"
out="$work/narrow.vpo"
timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" -p sce_vp_rsx \
    --emit-container "$out" "$src" >"$work/narrow.log" 2>&1 || {
    cat "$work/narrow.log" >&2
    fail "narrow shared-base vertex fixture did not compile"
}
python3 "$repo_root/tests/shader-compiler/vp_words.py" "$out" >"$work/narrow.words" || fail "invalid VP container"
grep -q 'dst=o7 mask=' "$work/narrow.words" || fail "narrow fixture has no TEXCOORD0 export"
if grep -qE 'dst=o7 mask=[xyzw]*[zw]' "$work/narrow.words"; then
    cat "$work/narrow.words" >&2
    fail "float2 base copy exported undeclared z/w lanes"
fi

printf 'PASS: selpred-output-alias-test\n'
