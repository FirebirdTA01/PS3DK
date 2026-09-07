#!/usr/bin/env bash
# `OUT o = (OUT)0;` FILLS EVERY LEAF WITH THE SCALAR.
#
# Semantic analysis refused the cast outright - "cannot cast from 'int' to
# 'struct'" - and clearing an output struct that way is the reference SDK's
# usual spelling.
#
# THE ROW COUNT, measured 2026-09-14 and not inherited: this construct moves
# ZERO rows of today's 920-row reference-SDK sweep.  The "16 rows" this guard
# used to cite came from the 2026-09-06 bucket survey; grepping every parent
# log on the current tip finds the diagnostic ZERO times.  Two corpus sources
# use the construct - CgTutorial/GCM/Metallic/shaders/MetallicFp.cg and
# util/Cg/ShaderOptimizer/cg/MetallicFp.cg, both `(PS_OUTPUT)0` - and both
# refuse EARLIER on the helper-default diagnostic (t_36492ad8), so they cannot
# flip until that lands.  This slice is therefore paid in advance: it unmasks,
# it does not move the census.  Do not restore a row count here without
# re-measuring it.
#
# The assertions are TWINS, not shapes: `(OUT)0` must produce the same
# container as writing float4(0,0,0,0) into the same field, on the zero, on a
# NON-zero fill, and through a nested leaf.  The non-zero row is what fails
# if the initialiser is dropped rather than emitted, since dropping a zero
# looks correct on a register that happens to be clear.
#
# WHAT THIS DELIBERATELY DOES NOT DO: the reference emits NOTHING for the
# zero-filled lanes that no later write touches (`float4 z = (float4)0;
# z.xy = p.xy;` is one MOV of xy there).  That is only sound if an unwritten
# register reads as zero, and RPCS3 initialises every fragment local itself
# (FragmentProgramDecompiler AddReg), so a same-boot pixel match cannot tell
# a hardware guarantee from an emulator convenience - and a physical register
# reused after another value's live range holds that value either way
# (codex, 2026-09-07).  So we emit the zeros and the parity gap is recorded.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-struct-scalar-init.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
decoder="$repo_root/tests/shader-compiler/fp_sources.py"

compile() {   # <stem> -> rc, container at $work/<stem>.fpo
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    rm -f "$work/$stem.fpo"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    [[ "$rc" -ne 124 ]] || fail "$stem timed out"
    return "$rc"
}

accept() {
    if ! compile "$1"; then
        tail -n 20 "$work/$1.err" >&2
        fail "$1 did not compile"
    fi
    [[ -s "$work/$1.fpo" ]] || fail "$1 wrote no container"
}

twin() {   # <cast stem> <explicit stem> <what>
    accept "$1"; accept "$2"
    if ! cmp -s "$work/$1.fpo" "$work/$2.fpo"; then
        python3 "$decoder" "$work/$1.fpo" | sed 's/^/  cast: /' >&2
        python3 "$decoder" "$work/$2.fpo" | sed 's/^/  twin: /' >&2
        fail "$3: the scalar-initialised struct and its explicit twin produced
different containers"
    fi
}

twin fp_struct_zero_init_f        fp_struct_zero_init_twin_f        "(OUT)0"
twin fp_struct_one_init_f         fp_struct_one_init_twin_f         "(OUT)1"
# The fill reaches a NESTED leaf too.  This row needs nested struct member
# access, which arrived in t_5386e484 (f36eb614) - on the commit before
# it the fixture fails at "no member named 'v' in 'INNER'", which is why
# the parent for this test is that landing rather than the one before.
twin fp_struct_nested_zero_init_f fp_struct_nested_zero_init_twin_f "(OUT)0 through a nested leaf"

# A NON-FIRST leaf, read after a non-zero fill.  Every row above reads a
# single-field struct or the FIRST field of a nested one, so a fill that stops
# after the first leaf is INVISIBLE to all of them - measured by Fable
# 2026-09-14 with a bindLeaves-breaks-after-first mutant that left all three
# twins above byte-identical while REFUSING this shape outright.
twin fp_struct_second_leaf_init_f fp_struct_second_leaf_init_twin_f "(OUT)1 read through the SECOND leaf"

# The fill must actually be IN the program: a twin pair that both dropped the
# initialiser would agree with each other.  The one-fill writes a constant
# into the lanes the shader never assigns.
python3 - "$work/fp_struct_one_init_f.fpo" "$decoder" <<'PY'
import re
import subprocess
import sys

rows = subprocess.run([sys.executable, sys.argv[2], sys.argv[1]],
                      capture_output=True, text=True, check=True).stdout
const = [l for l in rows.splitlines() if re.search(r"s0=c\d", l)]
if not const:
    raise SystemExit(
        "FAIL: (OUT)1 emitted no instruction reading a constant, so the fill "
        "was dropped rather than written.  The lanes the shader never assigns "
        "have to carry the 1.\n%s" % rows)
print("struct-scalar-init: the non-zero fill reaches the ucode (%d constant "
      "reads)" % len(const))
PY

# A SCALAR CANNOT INITIALISE A SAMPLER, so the cast stays refused for a
# struct that holds one - exit 1 exactly, and no container.
# `rc=$?` after an `if` reads the IF's status, not the command's - that is how
# this row first reported "refused with status 0" while the compiler had in
# fact refused.  Capture it from the call.
rc=0
compile fp_struct_sampler_init_refuse_f || rc=$?
[[ "$rc" -ne 0 ]] || fail "a struct holding a sampler accepted (HOLDER)0; the
rule is 'every leaf is numeric', not 'any struct'"
[[ "$rc" -eq 1 ]] || fail "the sampler struct refused with status $rc, expected 1
(124 is a timeout and >128 is a crash, neither of which is a refusal)"
[[ ! -s "$work/fp_struct_sampler_init_refuse_f.fpo" ]] \
    || fail "the sampler struct refused but still wrote a container"
grep -q "cannot cast" "$work/fp_struct_sampler_init_refuse_f.err" \
    || { tail -n 5 "$work/fp_struct_sampler_init_refuse_f.err" >&2
         fail "the sampler struct refused for the wrong reason"; }

# A LEAF WHOSE CONVERSION THIS SLICE DOES NOT DO IS REFUSED BY NAME.  The
# fill has to apply the LEAF's rule, and bool (a truth test), fixed (clamp
# to [-2, 2 - 2^-10]) and the narrow integers (wrap) each have their own.
# Writing the scalar through unchanged is a WRONG VALUE, measured against
# the reference: (S)0.5 into a bool leaf is 1 there and was 0.5 here, (S)3.0
# into a fixed leaf is 1.9990234375 there and was 3, (S)65537 into a short
# leaf is 1 there and was 65537 (codex, review of this commit).  These rows
# exist so the refusal cannot quietly become an acceptance again without
# someone doing the conversions.
for stem in fp_struct_init_bool_leaf_refuse_f \
            fp_struct_init_fixed_leaf_refuse_f \
            fp_struct_init_short_leaf_refuse_f \
            fp_struct_init_uint_leaf_refuse_f \
            fp_struct_init_uint_source_refuse_f \
            fp_struct_init_matrix_leaf_refuse_f \
            fp_struct_init_fixed_source_refuse_f \
            fp_struct_init_short_source_refuse_f; do
    rc=0
    compile "$stem" || rc=$?
    [[ "$rc" -ne 0 ]] || fail "$stem accepted: that leaf type needs its own
conversion and this slice does not do it, so the value would be the scalar
written through unchanged"
    [[ "$rc" -eq 1 ]] || fail "$stem refused with status $rc, expected 1"
    [[ ! -s "$work/$stem.fpo" ]] || fail "$stem refused but wrote a container"
    # Two diagnostics, because there are two gates: the leaf and source
    # kinds are refused in semantic analysis ("cannot cast"), and the
    # builder keeps a defensive check of its own for an unsigned source
    # that reaches it typed otherwise.
    grep -Eq "cannot cast|cannot fill a struct from an unsigned value" \
        "$work/$stem.err" \
        || { tail -n 5 "$work/$stem.err" >&2
             fail "$stem refused for the wrong reason"; }
done

printf 'struct-scalar-init: PASS\n'
