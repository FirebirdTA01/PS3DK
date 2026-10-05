#!/usr/bin/env bash
# A MISSING entry point must refuse cleanly on BOTH profiles (sce-cgc 475):
# with no -e flag the default entry "main" is not in the unit; the reference
# refuses with "entry point 'main' not found", writes no container, and the
# vertex profile must behave exactly like the fragment one - a crash (rc 139)
# or a missing diagnostic is not a refusal.  Measured on this toolchain at
# 6bb838dd: FP refused (rc 1), VP SEGFAULTED (rc 139) because
# validateVertexShader dereferenced the null entry for its warning location
# (t_83d32061).  A refusal must be EXACTLY exit 1 with the diagnostic and
# NO emitted container: a crash, a timeout, a container, or rc 0 are each a
# failure.
# usage: missing-entry-refusal-test.sh <rsx-cg-compiler>
set -u
cc="${1:?usage: $0 <rsx-cg-compiler>}"
sh="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)/tools/rsx-cg-compiler/tests/shaders/fp_c5122_nosem_f.cg"
[[ -f "$sh" ]] || { echo "missing-entry-refusal: FAIL: fixture not found: $sh" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0
fail() { echo "missing-entry-refusal: FAIL: $*" >&2; status=1; }

row() {  # row <refuse|accept> <name> <profile> <entry-or-empty> [diagnostic]
    local want="$1" name="$2" prof="$3" entry="${4:-}" diag="${5:-}"
    local rc out
    local extra=()
    if [ -n "$entry" ]; then extra=("-e" "$entry"); fi
    rm -f "$work/$name.bin"
    out=$(timeout 60 "$cc" -p "$prof" ${extra[@]+"${extra[@]}"} --emit-container "$work/$name.bin" "$sh" 2>&1)
    rc=$?
    if [ "$want" = refuse ]; then
        if [ "$rc" -ne 1 ]; then
            fail "$name: expected refusal exit 1, got $rc"
        elif [ -n "$diag" ] && ! printf '%s' "$out" | grep -Fq "$diag"; then
            fail "$name: refused without the expected diagnostic ($diag): $out"
        elif [ -e "$work/$name.bin" ]; then
            fail "$name: refused but left an output container behind"
        else
            echo "missing-entry-refusal: ok   $name refused"
        fi
    else
        if [ "$rc" -ne 0 ] || [ ! -s "$work/$name.bin" ]; then
            fail "$name: expected to compile, got exit $rc: $out"
        else
            echo "missing-entry-refusal: ok   $name compiles"
        fi
    fi
}

row refuse fp_missing_main sce_fp_rsx "" "entry point 'main' not found"
row refuse vp_missing_main sce_vp_rsx "" "entry point 'main' not found"
row accept fp_entry_beta   sce_fp_rsx "beta"
row refuse vp_entry_beta   sce_vp_rsx "beta" "Required output 'HPOS' not written"

[ "$status" -eq 0 ] && echo "missing-entry-refusal: PASS"
exit $status
