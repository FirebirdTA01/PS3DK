#!/usr/bin/env bash
# t_56ff2244: an if/else join must emit its Select instructions in an order
# taken from the SOURCE, not from a hash table.
#
# The defect: the join collected its variable names into a
# std::unordered_set<std::string> and iterated it, so the emission order was
# the standard library's string-hash bucket order.  An MSVC-built compiler and
# a gcc-built compiler therefore produced DIFFERENT PROGRAMS from the same
# source - measured 2026-09-06 on three of 421 corpus shaders.
#
# JUDGED ON THE IR, not on container bytes, and deliberately: this is an
# ORDERING defect, and an order can change while the container happens not to.
# All three source rows that moved were judged on the rig - two distinct binary
# pairs between them - and every one rendered identically, colour and depth.
# The bytes are the wrong instrument for the thing being guarded.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-join-order-test.$$"
mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# The ordered list of select TYPES, e.g. "vec3 float".
select_types() {
    local stem="$1"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" \
            "$compiler" -p sce_fp_rsx --dump-ir "$shaders/$stem.cg"
    ) >"$work/$stem.ir" 2>&1 || { tail -n 20 "$work/$stem.ir" >&2; fail "$stem did not compile"; }
    grep -oE '= select (vec[234]|float) ' "$work/$stem.ir" | awk '{print $3}' | tr '\n' ' '
}

base=$(select_types fp_join_order_decl_f)
swapped=$(select_types fp_join_order_decl_swapped_f)
renamed=$(select_types fp_join_order_renamed_f)

[[ "$base" == "vec3 float " ]] || fail "fp_join_order_decl_f declares colour first, so its vec3 select must come first; got: $base"
[[ "$swapped" == "float vec3 " ]] || fail "fp_join_order_decl_swapped_f declares depth first, so its float select must come first; got: $swapped"

# THE REGRESSION GUARD FOR THE DEFECT ITSELF.  The renamed copy differs from
# the baseline only in the two variable NAMES.  Under the defect the names
# decided the order, so this comparison was the thing that could not hold.
[[ "$renamed" == "$base" ]] || fail "renaming the join variables changed the emission order ($base vs $renamed): the join is keyed on names again (t_56ff2244)"

# THE TIE CASE.  Both names share one pre-if value, so the
# primary key - the smallest reaching SSA id - ties, and the NAME breaks it.
# Both selects are float, so the only observable in the dump is the operand
# order; on the fixed builder the then-operands ascend, and the parent emitted
# them descending, which is what makes this a discriminating row rather than a
# restatement.
select_types fp_join_order_shared_prevalue_f > /dev/null
mapfile -t tie < <(grep -oE '= select float %[0-9]+, %[0-9]+, %[0-9]+' "$work/fp_join_order_shared_prevalue_f.ir" | awk -F'%' '{print $3}' | tr -d ',')
[[ ${#tie[@]} -eq 2 ]] || fail "the shared-pre-value fixture must emit exactly two selects, got ${#tie[@]}"
[[ ${tie[0]} -lt ${tie[1]} ]] || fail "shared-pre-value join emitted its selects in ${tie[0]},${tie[1]} order; the tie must be broken by name and is not (t_56ff2244)"

printf 'join-order-test: ok (declaration order followed, rename does not move it, tie broken deterministically)\n'
printf 'PASS: join-order-test\n'
