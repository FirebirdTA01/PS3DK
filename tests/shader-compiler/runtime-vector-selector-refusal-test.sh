#!/usr/bin/env bash
# A RUN-TIME selector on a VECTOR is refused, as the reference does (sce-cgc
# 475: error C1011 "cannot index a non-array value").  Accepting it
# lowered the selector as lane 0, so t[s] silently returned t.x whatever s was.
# (CONSTANT float selectors on vectors are runtime-vector-index-test.sh's
# subject and stay accepted.)  Array elements and loop indices - constant once
# the loop is unrolled - stay accepted, as on the reference.  Every row was
# measured on sce-cgc 475 (2026-09-25).  A refusal must be EXACTLY exit 1 with
# the diagnostic: a crash or a timeout is not a refusal.
# usage: runtime-vector-selector-refusal-test.sh <rsx-cg-compiler>
set -u
cc="${1:?usage: $0 <rsx-cg-compiler>}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "runtime-vector-selector-refusal: FAIL: $*" >&2; status=1; }

VP_HEAD='void main(float4 p : POSITION, out float4 op : POSITION, out float4 c : COLOR0'

row() {  # row <refuse|accept> <name> <profile> <source>
    local want="$1" name="$2" prof="$3" src="$4" rc out
    printf '%s\n' "$src" > "$work/$name.cg"
    rm -f "$work/$name.bin"
    out=$(timeout 60 "$cc" -p "$prof" --emit-container "$work/$name.bin" "$work/$name.cg" 2>&1)
    rc=$?
    if [ "$want" = refuse ]; then
        if [ "$rc" -ne 1 ]; then
            fail "$name: expected refusal exit 1, got $rc"
        elif ! printf '%s' "$out" | grep -q "cannot index a non-array value"; then
            fail "$name: refused without the C1011 diagnostic: $out"
        elif [ -e "$work/$name.bin" ]; then
            fail "$name: refused but left an output container behind"
        else
            echo "runtime-vector-selector-refusal: ok   $name refused"
        fi
    else
        if [ "$rc" -ne 0 ] || [ ! -s "$work/$name.bin" ]; then
            fail "$name: expected to compile, got exit $rc: $out"
        else
            echo "runtime-vector-selector-refusal: ok   $name compiles"
        fi
    fi
}

row refuse fp_float_index   sce_fp_rsx 'float4 main(uniform float4 t, uniform float s) : COLOR { return float4(t[s]); }'
row refuse fp_int_index     sce_fp_rsx 'float4 main(uniform float4 t, uniform int s) : COLOR { return float4(t[s]); }'
row refuse fp_varying_index sce_fp_rsx 'float4 main(float4 tc : TEXCOORD0, uniform float4 t) : COLOR { return float4(t[tc.x]); }'
row refuse vp_float_index   sce_vp_rsx "$VP_HEAD, uniform float4 t, uniform float s) { op = p; c = float4(t[s]); }"
row refuse vp_int_index     sce_vp_rsx "$VP_HEAD, uniform float4 t, uniform int s) { op = p; c = float4(t[s]); }"
row refuse vp_varying_index sce_vp_rsx "$VP_HEAD, float4 tc : TEXCOORD0, uniform float4 t) { op = p; c = float4(t[tc.x]); }"
row refuse vp_matrix_row_element sce_vp_rsx "$VP_HEAD, uniform float4x4 m, uniform int s) { op = p; c = float4(m[0][s]); }"
row accept fp_constant_index sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { return float4(t[2.0]); }'
row accept fp_loop_index     sce_fp_rsx 'float4 main(uniform float4 t) : COLOR { float a = 0; for (int i = 0; i < 4; i++) a += t[i]; return float4(a); }'
row accept vp_constant_index sce_vp_rsx "$VP_HEAD, uniform float4 t) { op = p; c = float4(t[2.0]); }"
row accept vp_loop_index     sce_vp_rsx "$VP_HEAD, uniform float4 t) { op = p; float a = 0; for (int i = 0; i < 4; i++) a += t[i]; c = float4(a); }"
row accept vp_array_index    sce_vp_rsx "$VP_HEAD, uniform float4 a[4], uniform int s) { op = p; c = a[s]; }"

[ "$status" -eq 0 ] && echo "runtime-vector-selector-refusal: PASS"
exit $status
