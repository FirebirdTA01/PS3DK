#!/usr/bin/env bash
# t_5c1f84d3: texobj<K> is the Cg 1.x spelling of sampler<K>, and nothing else.
#
# Measured against the reference before any of this was written: for every
# one of the five names, the same program spelled sampler<K> and texobj<K>
# compiles to a BYTE-IDENTICAL container - sampled, and declared but never
# sampled, which is the SDK's commonest shape and the one that exercises the
# parameter table.  There is no second behaviour to discover; it is an alias.
#
# So the property asserted here is exactly that: the texobj spelling
# produces what the sampler spelling produces, whatever that is.  The test
# deliberately does NOT pin reference bytes - our sampled containers are not
# reference-identical today for reasons that have nothing to do with this
# row, and pinning them here would make an unrelated divergence look like
# this row's problem.
#
# THE NEGATIVE MATTERS AS MUCH AS THE POSITIVE: the reference REJECTS
# texobj2DShadow and texobjRECTShadow with a syntax error at the identifier.
# They are not types.  Adding the family by pattern rather than by
# measurement would invent two.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-texobj-alias-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# The sampling intrinsic and coordinate for each kind.
declare -A FN=( [1D]=tex1D [2D]=tex2D [3D]=tex3D [CUBE]=texCUBE [RECT]=texRECT )
declare -A UV=( [1D]="uv.x" [2D]="uv" [3D]="float3(uv, 0)" [CUBE]="float3(uv, 0)" [RECT]="uv" )

# Every sampling intrinsic in this table lowers today, so every sampled pair
# is compared on its CONTAINER BYTES.  3D was the last entry here that was
# not: tex3D refused with "unsupported IR op call" until bucket (c) lowered
# it, and this row's own stale-entry check is what said so - it failed with
# "sampler3D now COMPILES ... the pair should be compared on bytes" rather
# than quietly going on asserting a refusal that no longer happens.
#
# The mechanism stays because it is not about 3D: a kind whose intrinsic
# does not lower has both spellings compared on the REFUSAL instead, and the
# property under test is the same either way - the alias must not change
# whether a program is accepted, only how its type is spelled.
declare -A SAMPLED_COMPILES=( [1D]=yes [2D]=yes [3D]=yes [CUBE]=yes [RECT]=yes )

emit() {  # <path> <source-file>; echoes the exit status
    local out="$1" src="$2"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$out.log" 2>&1
    local status=$?
    set -e
    printf '%s' "$status"
}


# A REFUSAL IS NOT "a non-zero exit".  124 is a timeout, a signal is a crash,
# and either would pair up with the other spelling and read as agreement -
# the same trap the parser-progress guard is built around.  So a refusal here
# means exit EXACTLY 1, no container, and a diagnostic that says WHY.
#
# The reason is matched against the diagnostic body, not against the log:
# every one of these source files is named after the thing it is testing,
# so a compiler that merely echoes the path it was given would satisfy a
# search for the bare token.  Each pattern below therefore carries text
# only a real diagnostic has - the quoted form, or the lowering's own
# wording.  Echoes a problem description, or nothing.
check_refusal() {  # <compiler> <source> <out> <pattern>...
    local cc="$1" src="$2" out="$3" status pattern
    shift 3
    rm -f "$out"
    set +e
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$cc" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$out.log" 2>&1
    status=$?
    set -e
    if [[ "$status" != 1 ]]; then
        printf 'exited %s, expected exactly 1 (124 is a timeout and a signal is a crash; neither is a refusal)' "$status"
        return 0
    fi
    if [[ -f "$out" ]]; then
        printf 'refused but still wrote a container'
        return 0
    fi
    for pattern in "$@"; do
        if ! grep -qE "$pattern" "$out.log"; then
            printf 'refused without a diagnostic matching %s: %s' \
                "$pattern" "$(head -n 1 "$out.log")"
            return 0
        fi
    done
}

# Self-test the checker on a compiler that TIMES OUT and writes nothing - the
# shape that would otherwise be counted as a refusal.  A guard nobody has
# watched reject something is not a guard.
selftest_dir="$work/selftest"
mkdir -p "$selftest_dir"
printf '#!/usr/bin/env bash\nexit 124\n' > "$selftest_dir/always-timeout"
chmod +x "$selftest_dir/always-timeout"
printf 'void main(out float4 o : COLOR) { o = 1.0; }\n' > "$selftest_dir/probe.cg"
selftest_problem="$(check_refusal "$selftest_dir/always-timeout" "$selftest_dir/probe.cg" "$selftest_dir/probe.fpo" "anything")"
[[ -n "$selftest_problem" ]] || fail \
    "check_refusal accepted a compiler that exits 124 without compiling - the refusal arm of this test would count a hang as agreement"
case "$selftest_problem" in
    *"expected exactly 1"*) ;;
    *) fail "check_refusal rejected the timeout for the wrong reason: $selftest_problem" ;;
esac

# Second self-test: a compiler that refuses properly but for the WRONG
# REASON.  The stub exits 1, writes nothing, and prints a located error
# that names the source path - which is enough to satisfy a search for a
# bare token, because every one of these files is named after its subject.
printf '#!/usr/bin/env bash\nfor a in "$@"; do case "$a" in *.cg) src="$a";; esac; done\nprintf "%s:1:1: error: unrelated injected parser error\\n" "$src"\nexit 1\n' > "$selftest_dir/wrong-reason"
chmod +x "$selftest_dir/wrong-reason"
cp "$selftest_dir/probe.cg" "$selftest_dir/texobj2DShadow.cg"
selftest_problem="$(check_refusal "$selftest_dir/wrong-reason" "$selftest_dir/texobj2DShadow.cg" "$selftest_dir/wrong.fpo" "unknown type name .texobj2DShadow.")"
[[ -n "$selftest_problem" ]] || fail \
    "check_refusal accepted a refusal whose only mention of texobj2DShadow was the source path it was handed - the reason is not being matched against the diagnostic"

pairs=0
for kind in 1D 2D 3D CUBE RECT; do
    for shape in unused sampled; do
        for spelling in sampler texobj; do
            src="$work/${shape}_${spelling}_${kind}.cg"
            if [[ "$shape" == unused ]]; then
                printf 'void main(float3 c : COLOR0, out float3 o : COLOR, uniform %s%s t : TEXUNIT1)\n{\n    o = c;\n}\n' \
                    "$spelling" "$kind" > "$src"
            else
                # The tex1D slice refuses a surviving noncanonical binding
                # until explicit TEXUNIT allocation is repaired.  Exercise
                # accepted aliases on unit 0; the unused unit-1 row above
                # continues to check that an unused binding is accepted.
                unit=1
                [[ "$kind" == 1D ]] && unit=0
                printf 'void main(float2 uv : TEXCOORD0, out float4 o : COLOR, uniform %s%s t : TEXUNIT%d)\n{\n    o = %s(t, %s);\n}\n' \
                    "$spelling" "$kind" "$unit" "${FN[$kind]}" "${UV[$kind]}" > "$src"
            fi
        done

        s_out="$work/${shape}_sampler_${kind}.fpo"
        t_out="$work/${shape}_texobj_${kind}.fpo"
        s_status="$(emit "$s_out" "$work/${shape}_sampler_${kind}.cg")"
        t_status="$(emit "$t_out" "$work/${shape}_texobj_${kind}.cg")"

        [[ "$s_status" == "$t_status" ]] || {
            tail -n 3 "$t_out.log" >&2
            fail "$shape ${kind}: sampler$kind exited $s_status and texobj$kind exited $t_status - the spelling changed whether the program is accepted"
        }

        expect_ok=yes
        [[ "$shape" == sampled && "${SAMPLED_COMPILES[$kind]}" == no ]] && expect_ok=no

        if [[ "$expect_ok" == yes ]]; then
            [[ "$s_status" == 0 ]] || {
                tail -n 3 "$s_out.log" >&2
                fail "$shape ${kind}: the sampler CONTROL did not compile (exit $s_status); the fixture, not the alias, is wrong"
            }
            cmp -s "$s_out" "$t_out" || fail \
                "$shape ${kind}: sampler$kind and texobj$kind produced different containers - texobj is an alias, so the bytes must match, parameter table included"
            pairs=$((pairs + 1))
        else
            # A kind whose intrinsic does not lower yet: BOTH spellings
            # must refuse, in the same way, for the named reason.
            [[ "$s_status" == 0 ]] && fail \
                "sampled ${kind}: sampler$kind now COMPILES - ${FN[$kind]} started lowering, so this table entry is stale and the pair should be compared on bytes"
            for spelling in sampler texobj; do
                problem="$(check_refusal "$compiler" \
                    "$work/sampled_${spelling}_${kind}.cg" \
                    "$work/refusal_${spelling}_${kind}.fpo" \
                    "unsupported IR op call" "@${FN[$kind]}\)")"
                [[ -z "$problem" ]] || fail "sampled ${kind}, ${spelling}${kind}: $problem"
            done
            pairs=$((pairs + 1))
        fi
    done
done

# The two names the reference rejects.  A located diagnostic, no container.
for bogus in texobj2DShadow texobjRECTShadow; do
    src="$work/$bogus.cg"
    printf 'void main(float2 uv : TEXCOORD0, out float4 o : COLOR, uniform %s t : TEXUNIT1)\n{\n    o = float4(uv, 0, 1);\n}\n' \
        "$bogus" > "$src"
    out="$work/$bogus.fpo"
    problem="$(check_refusal "$compiler" "$src" "$out" "unknown type name .$bogus.")"
    [[ -z "$problem" ]] || fail \
        "$bogus: $problem - the reference rejects it with a syntax error at the identifier, so it is not a type and must not be added"
    grep -qE ":[0-9]+:[0-9]+: error:" "$out.log" || {
        tail -n 3 "$out.log" >&2
        fail "$bogus refused without a located diagnostic"
    }
done

printf 'texobj-alias-test: ok (%d sampler/texobj pairs agree; the two Shadow spellings are still refused)\n' "$pairs"
printf 'PASS: texobj-alias-test\n'
