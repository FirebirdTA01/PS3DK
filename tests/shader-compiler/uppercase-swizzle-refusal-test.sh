#!/usr/bin/env bash
# A UPPERCASE swizzle char is refused, as the reference does (sce-cgc 475:
# C1048 "invalid character 'X' in swizzle \"XYZ\"").  It is not an output
# -only rule: a READ swizzle (t.XYZ), a WRITE swizzle (c.XYZ = ...), and
# every family (XYZW, RGBA, STPQ, single letters, mixed case) is refused on
# the FIRST offending letter.  Lowercase xyz/rgb/stp all compile.  Every row
# was measured on sce-cgc 475 (2026-09-06, t_373d4005).  A refusal must be
# EXACTLY exit 1 with the diagnostic and NO emitted container: a crash, a
# timeout, or a container are not refusals.  The exit status of the reference
# varies run to run (0, 1 or 2 have all been recorded), so it is not a
# reference-valid guard; ours must still exit 1.
# usage: uppercase-swizzle-refusal-test.sh <rsx-cg-compiler>
set -u
cc="${1:?usage: $0 <rsx-cg-compiler>}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "uppercase-swizzle-refusal: FAIL: $*" >&2; status=1; }

VP_HEAD='void main(float4 p : POSITION, out float4 op : POSITION, out float4 c : COLOR0'

row() {  # row <refuse|accept> <name> <profile> <source> [diagnostic]
    local want="$1" name="$2" prof="$3" src="$4" diag="${5:-}" rc out
    printf '%s\n' "$src" > "$work/$name.cg"
    rm -f "$work/$name.bin"
    out=$(timeout 60 "$cc" -p "$prof" --emit-container "$work/$name.bin" "$work/$name.cg" 2>&1)
    rc=$?
    if [ "$want" = refuse ]; then
        if [ "$rc" -ne 1 ]; then
            fail "$name: expected refusal exit 1, got $rc"
        elif [ -n "$diag" ] && ! printf '%s' "$out" | grep -Fq "$diag"; then
            fail "$name: refused without the expected diagnostic ($diag): $out"
        elif [ -e "$work/$name.bin" ]; then
            fail "$name: refused but left an output container behind"
        else
            echo "uppercase-swizzle-refusal: ok   $name refused"
        fi
    else
        if [ "$rc" -ne 0 ] || [ ! -s "$work/$name.bin" ]; then
            fail "$name: expected to compile, got exit $rc: $out"
        else
            echo "uppercase-swizzle-refusal: ok   $name compiles"
        fi
    fi
}

row refuse fp_write_XYZ  sce_fp_rsx 'void main(out float4 c : COLOR) { c = 0; c.XYZ = float3(1,0,0); }'  'invalid character '"'"'X'"'"' in swizzle "XYZ"'
row refuse fp_read_W     sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { return float4(t.W); }'       'invalid character '"'"'W'"'"' in swizzle "W"'
row refuse fp_mixed_xY   sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { return t.xYxx; }'           'invalid character '"'"'Y'"'"' in swizzle "xYxx"'
row refuse fp_rgba_R     sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { return float4(t.R); }'       'invalid character '"'"'R'"'"' in swizzle "R"'
row refuse vp_stpq_S     sce_vp_rsx "$VP_HEAD, uniform float4 t) { op = p; c = float4(t.S); }"            'invalid character '"'"'S'"'"' in swizzle "S"'
row accept fp_lower      sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { return t.xyzw; }'
row accept fp_lower_rgba sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { return t.rgba; }'

[ "$status" -eq 0 ] && echo "uppercase-swizzle-refusal: PASS"
exit $status
