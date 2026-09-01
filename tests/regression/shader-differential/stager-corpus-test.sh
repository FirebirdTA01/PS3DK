#!/usr/bin/env bash
# stager-corpus-test.sh — the corpus half of the shader-differential
# stager must stage exactly the pairs worth judging, and must say so
# honestly.
#
# Runs stage-corpus.sh against the tracked compiler-test fixtures as a
# miniature corpus (they contain both compiling shaders and the
# permanent-refusal loop fixture) and asserts:
#   1. a refused shader lands in the ours-refused sidecar, per path,
#      and never in the manifest;
#   2. every manifest row references two existing, non-empty, byte-
#      DIFFERING containers (identical pairs are counted, not staged —
#      byte-identical implies pixel-identical, the rig's default gate);
#   3. rows carry the rig's 6-field shape with role=probe;
#   4. the helper refuses a zero-compile run (an empty corpus or a
#      broken compiler must not report as a clean sweep).
#
# The success control: the run must have compiled at least one shader
# and produced a summary line, or every later assertion is vacuous.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "$here/../../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-stager-corpus-test.$$"
mkdir -p "$work/out"
trap 'rm -rf "$work"' EXIT

fixtures="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# Run the helper over the fixture corpus.
rc=0
bash "$here/stage-corpus.sh" "$compiler" "$fixtures" "$work/out" \
    >"$work/run.log" 2>&1 || rc=$?
[[ "$rc" -eq 0 ]] || { tail -n 10 "$work/run.log" >&2; fail "stage-corpus.sh failed (rc=$rc)"; }

# Success control: a summary line with a nonzero compile count.
grep -Eq 'SDIFF-STAGE\|compiled=[1-9]' "$work/run.log" \
    || { tail -n 5 "$work/run.log" >&2; fail "no nonzero compiled= summary — zero-denominator run"; }

# 1. The permanent-refusal fixture is in the sidecar for both paths,
#    and absent from the manifest.
sidecar="$work/out/ours-refused.txt"
[[ -f "$sidecar" ]] || fail "ours-refused.txt sidecar missing"
grep -q 'fp_refusal_dynamic_loop_f.*default' "$sidecar" \
    || fail "loop fixture not recorded as refused on the default path"
grep -q 'fp_refusal_dynamic_loop_f.*general' "$sidecar" \
    || fail "loop fixture not recorded as refused on the general path"
manifest="$work/out/manifest-corpus.txt"
[[ -f "$manifest" ]] || fail "manifest-corpus.txt missing"
! grep -q 'fp_refusal_dynamic_loop_f' "$manifest" \
    || fail "refused shader leaked into the manifest"

# 2 + 3. Every manifest row: 6 |-fields, role=probe, both containers
#    exist, non-empty, and byte-DIFFER.
while IFS= read -r line; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    nf=$(awk -F'|' '{print NF}' <<<"$line")
    [[ "$nf" -eq 6 ]] || fail "manifest row has $nf fields, want 6: $line"
    role=$(cut -d'|' -f2 <<<"$line")
    [[ "$role" == "probe" ]] || fail "corpus row role is '$role', want probe: $line"
    a="$work/out/$(cut -d'|' -f4 <<<"$line")"
    b="$work/out/$(cut -d'|' -f5 <<<"$line")"
    [[ -s "$a" ]] || fail "row references missing/empty container: $a"
    [[ -s "$b" ]] || fail "row references missing/empty container: $b"
    if cmp -s "$a" "$b"; then
        fail "row stages a byte-IDENTICAL pair (should be counted, not staged): $line"
    fi
done <"$manifest"

# 4. Zero-compile refusal: an empty corpus directory must fail loudly.
mkdir -p "$work/empty" "$work/out2"
rc=0
bash "$here/stage-corpus.sh" "$compiler" "$work/empty" "$work/out2" \
    >"$work/empty.log" 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || fail "empty corpus reported success — zero-denominator disease"

printf 'stager-corpus-test: ok\n'
