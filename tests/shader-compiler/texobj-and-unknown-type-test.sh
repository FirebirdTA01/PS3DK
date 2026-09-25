#!/usr/bin/env bash
# Legacy texture-object types and unknown type names.
#
# The reference compiles texobj1D/2D/3D/CUBE/RECT byte-identically to the
# matching sampler type, in both profiles (a shipped reference sample
# declares `uniform texobj2D texture`).  It refuses bare `texobj` and any
# other unknown type name with a diagnostic.
#
# An unknown type name used to leave the parser retrying the same token and
# recording one error per attempt: the compiler grew until the allocator
# failed, and one run wrote a multi-gigabyte core dump.  Every compile here
# runs under an address-space cap and a timeout, so that failure mode ends
# in seconds as a FAIL instead of exhausting the machine.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
fail=0
compiled=" "
bad() { printf 'FAIL: %s\n' "$*" >&2; fail=1; }
ok() { printf 'ok   %s\n' "$*"; }
[[ -x "$compiler" ]] || { bad "rsx-cg-compiler not executable: $compiler"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# compile <profile> <source-file> <container> -> compiler exit status
compile() {
    ( ulimit -c 0; ulimit -v 1048576
      exec timeout 20 "$compiler" -p "$1" --emit-container "$3" "$2" ) \
        >"$work/last.log" 2>&1
}

# same_as_control <profile> <control-src> <alias-src> <ext> <alias> <sampler>:
# the alias must behave exactly as its sampler spelling.  Where we compile
# the control, the containers must be byte-identical; where we refuse the
# control (a separate gap, e.g. tex1D/tex3D or a vertex texture fetch
# today), the alias must refuse with the same exit status and message.
same_as_control() {
    local c_rc a_rc c_log
    compile "$1" "$2" "$work/c.$4"; c_rc=$?; c_log=$(cat "$work/last.log")
    compile "$1" "$3" "$work/a.$4"; a_rc=$?
    if [[ $c_rc -eq 0 && $a_rc -eq 0 ]]; then
        if cmp -s "$work/c.$4" "$work/a.$4"; then
            ok "$5 compiles byte-identically to $6 ($1)"
            compiled+="$5 "
        else
            bad "$5 container differs from $6 ($1)"
        fi
    elif [[ $c_rc -eq 1 && $a_rc -eq 1 && ! -e "$work/c.$4" && ! -e "$work/a.$4"             && "$c_log" == "$(cat "$work/last.log")" ]]; then
        # Only a clean refusal counts: exit 1, no container.  A shared abort
        # (134) or timeout (124) is two failures, not a match.
        ok "$5 is refused exactly as $6 is ($1, exit 1: a separate gap)"
    else
        bad "$5 exit $a_rc vs $6 exit $c_rc ($1): $(head -1 "$work/last.log")"
    fi
    rm -f "$work/c.$4" "$work/a.$4"
}

aliases=(
    "1D|sampler1D|tex1D(t, uv.x)"
    "2D|sampler2D|tex2D(t, uv)"
    "3D|sampler3D|tex3D(t, float3(uv, 0))"
    "CUBE|samplerCUBE|texCUBE(t, float3(uv, 1))"
    "RECT|samplerRECT|texRECT(t, uv)"
)
for row in "${aliases[@]}"; do
    IFS='|' read -r dim sampler call <<<"$row"
    printf 'float4 main(float2 uv : TEXCOORD0, uniform texobj%s t) : COLOR { return %s; }\n' \
        "$dim" "$call" >"$work/t.cg"
    printf 'float4 main(float2 uv : TEXCOORD0, uniform %s t) : COLOR { return %s; }\n' \
        "$sampler" "$call" >"$work/s.cg"
    same_as_control sce_fp_rsx "$work/s.cg" "$work/t.cg" fpo "texobj$dim" "$sampler"
done
# These spellings compile today and are the ones the reference sample and
# ordinary fragment programs use, so a refusal-for-refusal match is not
# enough for them.
for dim in 2D CUBE RECT; do
    [[ "$compiled" == *" texobj$dim "* ]] || bad "texobj$dim must compile, byte-identically to its sampler"
done

# Positive witness for every spelling in both profiles: an unused uniform
# of each type must compile, byte-identically to its sampler spelling, even
# where a fetch through that type is a separate gap.
for dim in 1D 2D 3D CUBE RECT; do
    sampler="sampler$dim"
    printf 'float4 main(float2 uv : TEXCOORD0, uniform texobj%s t) : COLOR { return float4(uv, 0, 1); }
' "$dim" >"$work/t.cg"
    printf 'float4 main(float2 uv : TEXCOORD0, uniform %s t) : COLOR { return float4(uv, 0, 1); }
' "$sampler" >"$work/s.cg"
    same_as_control sce_fp_rsx "$work/s.cg" "$work/t.cg" fpo "unused-texobj$dim" "$sampler"
    printf 'float4 main(float4 p : POSITION, uniform texobj%s t) : POSITION { return p; }
' "$dim" >"$work/t.cg"
    printf 'float4 main(float4 p : POSITION, uniform %s t) : POSITION { return p; }
' "$sampler" >"$work/s.cg"
    same_as_control sce_vp_rsx "$work/s.cg" "$work/t.cg" vpo "unused-vertex-texobj$dim" "$sampler"
    for witness in "unused-texobj$dim" "unused-vertex-texobj$dim"; do
        [[ "$compiled" == *" $witness "* ]] || bad "$witness must compile, byte-identically to $sampler"
    done
done

# Vertex profile: a vertex texture fetch through texobj2D.
printf 'float4 main(float4 p : POSITION, uniform texobj2D t) : POSITION { return tex2D(t, p.xy); }\n' >"$work/tv.cg"
printf 'float4 main(float4 p : POSITION, uniform sampler2D t) : POSITION { return tex2D(t, p.xy); }\n' >"$work/sv.cg"
same_as_control sce_vp_rsx "$work/sv.cg" "$work/tv.cg" vpo "vertex-texobj2D" sampler2D

# Unknown type names in every declaration position must be refused with a
# diagnostic naming the type: exit 1, no container, bounded time and memory.
unknown=(
    "global|foo|foo g; float4 main() : COLOR { return float4(0, 0, 0, 0); }"
    "parameter|foo|float4 main(float2 uv : TEXCOORD0, foo t) : COLOR { return float4(0, 0, 0, 0); }"
    "uniform|foo|float4 main(uniform foo t) : COLOR { return float4(0, 0, 0, 0); }"
    "local|foo|float4 main() : COLOR { foo x; return float4(0, 0, 0, 0); }"
    "bare-texobj|texobj|float4 main(float2 uv : TEXCOORD0, uniform texobj t) : COLOR { return float4(0, 0, 0, 0); }"
    "texobj2DARRAY|texobj2DARRAY|float4 main(float2 uv : TEXCOORD0, uniform texobj2DARRAY t) : COLOR { return float4(0, 0, 0, 0); }"
)
for row in "${unknown[@]}"; do
    IFS='|' read -r name type_name src <<<"$row"
    printf '%s\n' "$src" >"$work/u.cg"
    rm -f "$work/u.fpo"
    compile sce_fp_rsx "$work/u.cg" "$work/u.fpo"; rc=$?
    if [[ $rc -ne 1 ]]; then
        bad "$name: exit $rc, expected 1 ($(tail -1 "$work/last.log"))"
    elif [[ -e "$work/u.fpo" ]]; then
        bad "$name: refusal left a container behind"
    elif ! grep -q "error: .*'$type_name'" "$work/last.log"; then
        bad "$name: no error naming '$type_name': $(head -1 "$work/last.log")"
    else
        ok "$name: unknown type '$type_name' refused with a diagnostic"
    fi
done

# Error-cap backstop: many independent bad declarations stop at the cap
# with the cap message instead of running away.
for i in $(seq 1 200); do printf 'foo g%d;\n' "$i"; done >"$work/many.cg"
printf 'float4 main() : COLOR { return float4(0, 0, 0, 0); }\n' >>"$work/many.cg"
compile sce_fp_rsx "$work/many.cg" "$work/many.fpo"; rc=$?
if [[ $rc -eq 1 ]] && grep -q "Too many errors" "$work/last.log"; then
    ok "200 bad declarations stop at the error cap"
else
    bad "error cap: exit $rc, $(tail -1 "$work/last.log")"
fi

if [[ $fail -ne 0 ]]; then
    echo "texobj-and-unknown-type: FAIL"
    exit 1
fi
echo "texobj-and-unknown-type: PASS"
