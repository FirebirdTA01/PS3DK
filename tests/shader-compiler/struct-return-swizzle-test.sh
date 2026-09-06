#!/usr/bin/env bash
# t_c1d781ba: a struct member written through .rgb/.a (or .stp/.q) swizzles is
# the SAME lane group as .xyz/.w.  A struct member is addressed through a
# composite STRING key (field + member), which bypassed the swizzle decoders
# the rest of the frontend uses: the write stored the author's spelling and the
# return reconstruction hard-coded ".xyz"/".w", so .rgb/.a writes were missed,
# the store dropped and the value math dead-code-eliminated - a silent
# miscompile accepted with exit 0.
#
# THE FIX canonicalises a confirmed vector swizzle to its xyzw spelling wherever
# a composite key is built, so .rgb and .xyz compose one key and the map holds
# the last write by construction.  It covers the LOWERCASE rgba/stpq spellings
# the reference treats as equivalent; an uppercase swizzle is left refusing, as
# the reference refuses it (C1048), and is tracked in t_373d4005.
#
# THE GUARD IS ALIAS-SPELLING EQUIVALENCE WITH WRITE ORDER PRESERVED, not
# order-insensitivity: spelling is free, write order is not.  Assertions:
#  - .rgb/.a equals .xyz/.w (the dropped store);
#  - a cached existing value assigned last wins regardless of spelling, and
#    DIFFERS from the same program without that last write (the shape that
#    proved a max-SSA-id tie-break wrong - value ids order creation, not
#    assignment), on the xyz AND w/a lanes;
#  - the two write ORDERS differ on both lane groups (assignments do not
#    commute - a canonicalisation that merged too eagerly would make order
#    irrelevant and pass the equivalence checks while breaking the language);
#  - a .rgb write in one conditional arm survives the buildIfStmt merge;
#  - struct FIELDS named rgb/a stay distinct from each other - a live output
#    reading both, versus a control that merges them, must differ.
#
# Two shapes that would exercise the read-side builder through a compiled
# program - reading a struct-member vector back in arithmetic, and an
# out-parameter written in an inlined function - do not lower on the general
# path today (they refuse identically on the parent), so those builders are
# covered by the source audit rather than a run.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"
work="${TMPDIR:-/tmp}/ps3dk-struct-return-swizzle-test.$$"
mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
compile() {
    local stem="$1"; set +e
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" -p sce_fp_rsx \
          --emit-container "$work/$stem.fpo" "$shaders/$stem.cg" ) >"$work/$stem.log" 2>&1
    local rc=$?; set -e
    [[ $rc -eq 0 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem did not compile (exit $rc)"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem compiled but wrote no container"
}
same() { cmp -s "$work/$1.fpo" "$work/$2.fpo" || fail "$1 differs from $2 - $3"; printf '  %-36s == %s\n' "$1" "$2"; }
differ() { ! cmp -s "$work/$1.fpo" "$work/$2.fpo" || fail "$1 identical to $2 - $3"; printf '  %-36s != %s\n' "$1" "$2"; }

for s in fp_struct_return_rgba_f fp_struct_return_xyzw_control_f \
         fp_struct_return_cached_rgb_f fp_struct_return_cached_xyz_control_f fp_struct_return_cached_first_f \
         fp_struct_return_cached_wa_rgb_f fp_struct_return_cached_wa_w_control_f \
         fp_struct_return_order_ab_f fp_struct_return_order_ba_f \
         fp_struct_return_order_wa_ab_f fp_struct_return_order_wa_ba_f \
         fp_struct_return_branch_f fp_struct_return_branch_control_f \
         fp_struct_realfield_f fp_struct_realfield_control_f fp_struct_realfield_merged_f \
         fp_struct_realfield_a_f fp_struct_realfield_a_control_f fp_struct_realfield_a_merged_f; do
    compile "$s"
done

same   fp_struct_return_rgba_f fp_struct_return_xyzw_control_f "the .rgb/.a struct return must emit the same store as .xyz/.w"
same   fp_struct_return_cached_rgb_f fp_struct_return_cached_xyz_control_f "the last write (an existing var) wins regardless of spelling"
differ fp_struct_return_cached_rgb_f fp_struct_return_cached_first_f "the surviving write is the LAST one, not the earlier computed one"
same   fp_struct_return_cached_wa_rgb_f fp_struct_return_cached_wa_w_control_f "cached last-write holds on the w/a lane too"
differ fp_struct_return_order_ab_f fp_struct_return_order_ba_f "xyz/rgb assignments do not commute - last write wins"
differ fp_struct_return_order_wa_ab_f fp_struct_return_order_wa_ba_f "w/a assignments do not commute either (a single lane hides an over-eager merge)"
same   fp_struct_return_branch_f fp_struct_return_branch_control_f "a .rgb write in one conditional arm survives the merge like .xyz"
same   fp_struct_realfield_f fp_struct_realfield_control_f "struct fields named rgb/xyz are not swizzles - renaming them changes nothing"
differ fp_struct_realfield_f fp_struct_realfield_merged_f "merging the two real fields WOULD change the output - the guard sees it"
same   fp_struct_realfield_a_f fp_struct_realfield_a_control_f "scalar fields named a/w are not swizzles either"
differ fp_struct_realfield_a_f fp_struct_realfield_a_merged_f "merging the two real scalar fields WOULD change the output"

printf 'struct-return-swizzle-test: ok (alias spelling equivalent both lanes, last write pinned, write order preserved, branch-merged, real fields kept distinct)\n'
printf 'PASS: struct-return-swizzle-test\n'
