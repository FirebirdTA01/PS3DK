#!/usr/bin/env bash
# output-store-lane-test.sh - a scalar or narrow vector stored DIRECTLY to the
# colour output must read the lanes the source selected (t_cd76485f).
#
# `return p.z` and `o = p.z` painted p.x.  The widening branch in
# lowerStoreOutput overwrote the source's swizzle with {0,0,0,0} instead of
# replicating the lane resolve() had already selected, so these four programs
# compiled to IDENTICAL containers:
#
#     return p.x;   return p.y;   return p.z;   return p.w;
#
# The same branch handles widths 2 and 3 with {0,1,0,0} and {0,1,2,0}, so
# `o = p.zw` wrote p.xy and `o = p.yzw` wrote p.xyz.  Measured against the
# reference, which composes: .zwzz and .yzww for those two.
#
# WHY NOTHING ELSE CATCHES IT.  scalar-ctor-broadcast-test pins the source
# lane for the CONSTRUCTOR path - its M2 mutation is this defect one path over
# - and that path was already correct.  return-output-test and
# colour-reaches-r0-test asserts the store reaches R0, not which lane it
# reads.  A lane error is invisible to every check that looks at the
# destination.
#
# WHAT IS PINNED, and it is deliberately not byte parity: that the WRITTEN
# lanes read the right source components, asserted two ways - two spellings
# that must differ from each other, and the decoded source swizzle.  What the
# reference puts in the lanes the mask does NOT write is not one rule
# (`o = p.zw` gives .zwzz, `o = p.yzw` gives .yzww) and cannot change a
# rendered pixel; that stays a named byte-parity gap rather than a guess.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# A refusal is exit 1 exactly; 124 is a timeout and >= 128 a signal, and
# either satisfies "not zero" while meaning the compiler never decided
# (t_fd95d1b9).
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

work="${TMPDIR:-/tmp}/ps3dk-output-store-lane.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
decoder="$here/fp_sources.py"
[[ -f "$decoder" ]] || fail "fp_sources.py is missing: $decoder"

emit() {   # <stem> <source>
    printf '%s\n' "$2" >"$work/$1.cg"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$1.fpo" "$work/$1.cg"
    ) >"$work/$1.log" 2>&1 || rc=$?
    refusal_status "$rc" "$1"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$1.log" >&2; fail "$1 did not compile"; }
    [[ -s "$work/$1.fpo" ]] || fail "$1 compiled but wrote no container"
    python3 "$decoder" "$work/$1.fpo" >"$work/$1.ins"
}

# <stem> <extended regex> <what it means>
want() {
    grep -qE "$2" "$work/$1.ins" \
        || { cat "$work/$1.ins" >&2; fail "$1: no instruction matching '$2' - $3"; }
}

# <a> <b> <what it means>
must_differ() {
    cmp -s "$work/$1.fpo" "$work/$2.fpo" \
        && { cat "$work/$1.ins" >&2
             fail "$1 and $2 compiled to IDENTICAL containers - $3"; }
    return 0
}

# ---- a scalar into a float4 colour output ----------------------------------
for lane in x y z w; do
    emit "ret_$lane" "float4 main(float4 p : TEXCOORD0) : COLOR { return p.$lane; }"
    want "ret_$lane" "^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0[.]$lane$lane$lane$lane\$" \
        "the returned scalar must broadcast the lane it selected"
done
must_differ ret_x ret_y "a returned p.y must not compile to the same program as p.x"
must_differ ret_x ret_z "a returned p.z must not compile to the same program as p.x"
must_differ ret_x ret_w "a returned p.w must not compile to the same program as p.x"
printf '  %-38s %s\n' "return p.<lane>" "four distinct programs, each broadcasting its own lane"

# ---- the out-parameter spelling, which is the other natural one -------------
emit out_x 'void main(float4 p : TEXCOORD0, out float4 o : COLOR) { o = p.x; }'
emit out_z 'void main(float4 p : TEXCOORD0, out float4 o : COLOR) { o = p.z; }'
want out_z '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0[.]zzzz$' \
    "an out-parameter store must broadcast the selected lane too"
must_differ out_x out_z "o = p.z must not compile to the same program as o = p.x"
printf '  %-38s %s\n' "o = p.<lane>, out parameter" "distinct, and .z broadcasts z"

# ---- narrower outputs: the same branch, widths 2 and 3 ----------------------
emit w2_xy  'void main(float4 p : TEXCOORD0, out float2 o : COLOR) { o = p.xy; }'
emit w2_zw  'void main(float4 p : TEXCOORD0, out float2 o : COLOR) { o = p.zw; }'
want w2_zw '^[0-9]+ MOV dst=R0 mask=xy .* s0=TEX0[.]zw' \
    "a float2 store must read the two lanes it named, not lanes x and y"
must_differ w2_xy w2_zw "o = p.zw must not compile to the same program as o = p.xy"
emit w3_xyz 'void main(float4 p : TEXCOORD0, out float3 o : COLOR) { o = p.xyz; }'
emit w3_yzw 'void main(float4 p : TEXCOORD0, out float3 o : COLOR) { o = p.yzw; }'
want w3_yzw '^[0-9]+ MOV dst=R0 mask=xyz .* s0=TEX0[.]yzw' \
    "a float3 store must read the three lanes it named"
must_differ w3_xyz w3_yzw "o = p.yzw must not compile to the same program as o = p.xyz"
printf '  %-38s %s\n' "narrow outputs, widths 2 and 3" "the named lanes are what is read"

# ... and the same widths with the lanes REVERSED, because the rule this
# commit implements - compose with the resolved swizzle - came from two
# reference measurements that were both CONTIGUOUS ASCENDING RUNS (.zw and
# .yzw).  A rule that only handled ascending runs would give the right
# lanes for those two and the wrong ones here.  Measured against the
# reference: `o = p.wz` reads .wz, `o = p.zyx` reads .zyx, and reversed,
# repeated and permuted selections all compose - six samples (t_cd76485f).
emit w2_wz  'void main(float4 p : TEXCOORD0, out float2 o : COLOR) { o = p.wz; }'
want w2_wz '^[0-9]+ MOV dst=R0 mask=xy .* s0=TEX0[.]wz' \
    "a reversed float2 store must read w then z, not z then w"
must_differ w2_zw w2_wz "o = p.wz selects the same two lanes in the OTHER order and cannot be the same program"
emit w3_zyx 'void main(float4 p : TEXCOORD0, out float3 o : COLOR) { o = p.zyx; }'
want w3_zyx '^[0-9]+ MOV dst=R0 mask=xyz .* s0=TEX0[.]zyx' \
    "a reversed float3 store must read z, y, x in that order"
must_differ w3_xyz w3_zyx "o = p.zyx must not compile to the same program as o = p.xyz"
printf '  %-38s %s\n' "reversed lane order, widths 2 and 3" "composition is not an ascending-run special case"

# ---- the spellings that were ALREADY right, kept as regressions -------------
# Each of these routes the lane through something else before the store, and
# each was correct before this fix; they are here so a future change to the
# store path cannot break them while the rows above still pass.
emit via_local 'float4 main(float4 p : TEXCOORD0) : COLOR { float s = p.z; return float4(s, s, s, s); }'
emit via_ctor  'float4 main(float4 p : TEXCOORD0) : COLOR { return float4(p.z); }'
emit via_mul   'float4 main(float4 p : TEXCOORD0) : COLOR { return p * p.z; }'
for stem in via_local via_ctor via_mul; do
    grep -qE 's0=TEX0[.]zzzz|s1=TEX0[.]zzzz' "$work/$stem.ins" \
        || { cat "$work/$stem.ins" >&2; fail "$stem lost the z lane; it reached the store through a temp and was correct before t_cd76485f"; }
done
printf '  %-38s %s\n' "local / constructor / arithmetic" "still carry the lane"

# ---- and the check itself, seen accusing ------------------------------------
# must_differ is the load-bearing assertion here, so it is shown refusing:
# a file compared with ITSELF must be reported as identical.
if ( must_differ ret_z ret_z "self-comparison" ) 2>/dev/null; then
    fail "must_differ passed a file compared with itself - it would pass two identical containers"
fi
printf '  %-38s %s\n' "self-check" "must_differ refuses a file compared with itself"

printf 'PASS: output-store-lane-test\n'
