#!/usr/bin/env bash
# t_f9ecd3ac: array uniforms with CONSTANT indices, parameter and file-scope,
# fragment and vertex, laid out one element at a time the way the reference
# lays them out.  A VERTEX run-time index is the next slice's contiguous
# block (vp-array-dynamic-test.sh); a FRAGMENT one keeps its named refusal.
#
# The oracle is the reference's PARAMETER TABLE, measured on sce-cgc and
# pinned here as literal expectations (the reference itself cannot run in
# CI): one record per element named `name[i]`, typed as the element (1048,
# float4 - not the scalar 1045 the old converter declared), in declaration
# order, used or not; every element shares the parameter's SOURCE ordinal
# as paramno (or -1 at file scope); a used FP element carries its own
# inline-const relocation offsets, DESCENDING, and isReferenced=1; a used VP
# element takes a constant register DESCENDING from c467 in ASCENDING
# element order, and an unused one is declared with no register at all.
#
# Before this, an array reached the lowering as ONE scalar record: a
# parameter array fell through to VecExtract and read a LANE of a single
# float4 as the element (exit 0, wrong pixels - the e88 miscompile), and a
# file-scope array refused.  Every accept row below is red on that parent.
#
# The refusals are the half that keeps the slice honest: a fragment
# run-time index refuses (no indexed constants; C6013), out-of-range and
# invalid constant indices refuse (the reference refuses them too: C1068,
# C6013), and an element type this slice does not lay out refuses by name.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-array-uniform.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

emit() {  # <stem> <profile> -> exit status; container at $work/<stem>.bin
    local stem="$1" profile="$2"
    rm -f "$work/$stem.bin"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p "$profile" --emit-container "$work/$stem.bin" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$stem timed out"
    printf '%s' "$rc"
}
profile_of() { case "$1" in *_v) printf 'sce_vp_rsx' ;; *) printf 'sce_fp_rsx' ;; esac; }
accept() {  # <stem>
    local rc; rc="$(emit "$1" "$(profile_of "$1")")"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$1.log" >&2; fail "$1 exited $rc, expected 0"; }
    [[ -s "$work/$1.bin" ]] || fail "$1 compiled but wrote no container"
}

# params <stem>: the container's parameter table, one line per record, in
# the WRITER's column order (cg_container_fp.cpp: type res var resIndex
# name defaultValue embeddedConstants semantic direction paramno
# isReferenced isShared; 48 bytes each).  Prints:
#     <name> type=<t> paramno=<p> ref=<0|1> resIndex=<r> embedded=<n>[:<offsets...>]
params() {
    python3 - "$work/$1.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
_, _, _, count, table, _, _, _ = struct.unpack_from('>8I', b, 0)
def cstr(off):
    if off == 0 or off >= len(b): return ''
    return b[off:b.index(b'\0', off)].decode('latin1')
for i in range(count):
    typ, res, var, resindex, name, _, emb, _, _, paramno, isref, _ = \
        struct.unpack_from('>12I', b, table + i * 48)
    offs = []
    if emb and emb < len(b):
        n = struct.unpack_from('>I', b, emb)[0]
        offs = list(struct.unpack_from('>%dI' % n, b, emb + 4))
    print('%s type=%d paramno=%s ref=%d resIndex=%s embedded=%d%s' % (
        cstr(name), typ, 'none' if paramno == 0xffffffff else str(paramno), isref,
        'none' if resindex == 0xffffffff else hex(resindex), len(offs),
        ':' + ','.join(str(o) for o in offs) if offs else ''))
PY
}
expect_record() {  # <stem> <regex>  (one full line must match)
    grep -qE "^$2\$" "$work/$1.params" \
        || { cat "$work/$1.params" >&2; fail "$1: no parameter record matching '$2'"; }
}
element_order() {  # <stem> <name> <count>: the N element records appear consecutively, in order
    local stem="$1" name="$2" count="$3" i
    # `|| true`: a grep that matches nothing exits 1, and under set -e an
    # unguarded substitution kills the script BEFORE the assertion below can
    # say what was expected (a review saw a red with zero bytes of output).
    local got; got="$(grep -oE "^${name//[/\\[}\[[0-9]+\]" "$work/$stem.params" | tr '\n' ' ' || true)"
    local want=""; for ((i = 0; i < count; i++)); do want+="${name}[$i] "; done
    [[ "$got" == "$want" ]] || { cat "$work/$stem.params" >&2; fail "$stem: elements of $name are '$got', expected '$want' (one record per element, in order)"; }
}

# ---------------------------------------------------------------------------
# FILE-SCOPE FP ARRAY, one element used (u_colors[2] of 4).
accept fp_array_uniform_literal_f
params fp_array_uniform_literal_f > "$work/fp_array_uniform_literal_f.params"
element_order fp_array_uniform_literal_f u_colors 4
expect_record fp_array_uniform_literal_f 'u_colors\[0\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_literal_f 'u_colors\[1\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_literal_f 'u_colors\[2\] type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_literal_f 'u_colors\[3\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
grep -qE '^u_colors ' "$work/fp_array_uniform_literal_f.params" && fail "the array itself has a record; the reference declares only elements"
printf '  %-40s 4 element records, [2] referenced\n' "FP file-scope u_colors[2]"

# A FOLDED constant index is the same element: u_colors[1 + 1] byte-equals u_colors[2].
accept fp_array_uniform_folded_f
cmp -s "$work/fp_array_uniform_folded_f.bin" "$work/fp_array_uniform_literal_f.bin" \
    || fail "u_colors[1 + 1] did not compile to the bytes of u_colors[2] - the constant index was not evaluated"
printf '  %-40s == literal\n' "FP u_colors[1 + 1]"

# CASTS in a constant index APPLY the target's semantics (a review found a
# draft passing the operand through: (int)(bool)2 read element 2 where the
# reference reads element 1).  Measured on the reference: (int)(bool)2 ==
# u[1], (short)65538 == u[2], (int)2.5 == u[2], and a float-TYPED index
# u[(float)2] == u[2] (accepted since t_050bebce, as the reference does).
accept fp_array_uniform_index1_f
accept fp_array_uniform_cast_bool_f
cmp -s "$work/fp_array_uniform_cast_bool_f.bin" "$work/fp_array_uniform_index1_f.bin" \
    || fail "u_colors[(int)(bool)2] did not compile to the bytes of u_colors[1] - the bool cast was not applied"
accept fp_array_uniform_cast_short_f
cmp -s "$work/fp_array_uniform_cast_short_f.bin" "$work/fp_array_uniform_literal_f.bin" \
    || fail "u_colors[(short)65538] did not compile to the bytes of u_colors[2] - the narrowing cast was not applied"
accept fp_array_uniform_cast_floatlit_f
cmp -s "$work/fp_array_uniform_cast_floatlit_f.bin" "$work/fp_array_uniform_literal_f.bin" \
    || fail "u_colors[(int)2.5] did not compile to the bytes of u_colors[2] - the float literal was not truncated under the integral cast"
# A FLOAT-TYPED index is accepted with int() semantics (t_050bebce; the
# reference accepts it): u_colors[(float)2] is element 2.
accept fp_array_uniform_cast_float_f
cmp -s "$work/fp_array_uniform_cast_float_f.bin" "$work/fp_array_uniform_literal_f.bin" \
    || fail "u_colors[(float)2] did not compile to the bytes of u_colors[2] - a float-typed constant index must truncate to the element"
printf '  %-40s bool -> 1, short wraps, (int)2.5 -> 2\n' "FP casts in a constant index"

# A name bound in scope SHADOWS a global array of the same name: the
# parameter `float4 u` and the local `float4 u = p` are that varying, and
# u[2] is its lane, never the global's element (a review found a draft
# reading the global).  Pinned on the UCODE: byte-identical to the
# unshadowed control's, while the container still declares the global's
# four elements, all unreferenced - which is also what the reference does.
ucode_bytes() {  # <stem> -> the ucode region, raw, on stdout
    python3 - "$work/$1.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
_, _, _, _, _, _, ucsz, uc = struct.unpack_from('>8I', b, 0)
sys.stdout.buffer.write(b[uc:uc + ucsz])
PY
}
accept fp_array_shadow_control_f
ucode_bytes fp_array_shadow_control_f > "$work/shadow_control.ucode"
for stem in fp_array_shadow_param_f fp_array_shadow_local_f; do
    accept "$stem"
    ucode_bytes "$stem" > "$work/$stem.ucode"
    cmp -s "$work/$stem.ucode" "$work/shadow_control.ucode" \
        || fail "$stem: ucode differs from the unshadowed control's - the shadowed name resolved to the global array"
    params "$stem" > "$work/$stem.params"
    element_order "$stem" u 4
    for k in 0 1 2 3; do
        expect_record "$stem" "u\[$k\] type=1048 paramno=none ref=0 resIndex=none embedded=0"
    done
done
printf '  %-40s ucode == control, global declared unreferenced\n' "shadowed global array (param, local)"

# One element used TWICE keeps BOTH relocation offsets, DESCENDING; another
# used once; two unused but declared.
accept fp_array_uniform_two_f
params fp_array_uniform_two_f > "$work/fp_array_uniform_two_f.params"
expect_record fp_array_uniform_two_f 'u_colors\[0\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_two_f 'u_colors\[1\] type=1048 paramno=none ref=1 resIndex=none embedded=2:[0-9]+,[0-9]+'
expect_record fp_array_uniform_two_f 'u_colors\[2\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_two_f 'u_colors\[3\] type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
offs="$(grep -oE '^u_colors\[1\].*embedded=2:[0-9]+,[0-9]+' "$work/fp_array_uniform_two_f.params" | sed 's/.*embedded=2://' || true)"
[[ -n "$offs" ]] || fail "u_colors[1] has no two-offset record to read the order from"
[[ "${offs%%,*}" -gt "${offs##*,}" ]] || fail "u_colors[1]'s two relocation offsets are '$offs', expected descending order (the writer sorts them so, and the runtime patches in that sequence)"
printf '  %-40s [1] two offsets descending, [3] one\n' "FP u_colors[1] twice, [3] once"

# Arrays beside a scalar uniform: records in declaration order, each array
# expanded in place, referenced flags per element.
accept fp_array_uniform_beside_scalar_f
params fp_array_uniform_beside_scalar_f > "$work/fp_array_uniform_beside_scalar_f.params"
uniforms="$(grep -oE '^u_[a-z]+(\[[0-9]+\])?' "$work/fp_array_uniform_beside_scalar_f.params" | tr '\n' ' ' || true)"
[[ "$uniforms" == "u_tint u_colors[0] u_colors[1] u_other[0] u_other[1] u_other[2] " ]] \
    || fail "uniform records are '$uniforms', expected declaration order with each array expanded in place"
expect_record fp_array_uniform_beside_scalar_f 'u_tint type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_beside_scalar_f 'u_colors\[1\] type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_beside_scalar_f 'u_other\[1\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_beside_scalar_f 'u_other\[2\] type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
printf '  %-40s declaration order, per-element flags\n' "FP scalar + two arrays"

# PARAMETER arrays (the SDK's usual spelling and the e88 miscompile): every
# element carries the parameter's SOURCE ordinal, later parameters keep
# their own ordinals, and the slot numbering past the array does not
# collide - u_scale (ordinal 2) and u_more[1] (ordinal 3) each get their
# own relocation offsets, as does the file-scope u_global after them.
accept fp_array_uniform_param_f
params fp_array_uniform_param_f > "$work/fp_array_uniform_param_f.params"
uniforms="$(grep -oE '^u_[a-z]+(\[[0-9]+\])?' "$work/fp_array_uniform_param_f.params" | tr '\n' ' ' || true)"
[[ "$uniforms" == "u_colors[0] u_colors[1] u_colors[2] u_scale u_more[0] u_more[1] u_global " ]] \
    || fail "parameter-array records are '$uniforms'"
expect_record fp_array_uniform_param_f 'u_colors\[0\] type=1048 paramno=1 ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_param_f 'u_colors\[2\] type=1048 paramno=1 ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_param_f 'u_scale type=1048 paramno=2 ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_param_f 'u_more\[0\] type=1048 paramno=3 ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_param_f 'u_more\[1\] type=1048 paramno=3 ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_param_f 'u_global type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
# Four distinct relocation offsets: a slot collision would make two of
# them equal, and a runtime patch of one uniform would land in another.
offs="$(grep -oE 'embedded=1:[0-9]+' "$work/fp_array_uniform_param_f.params" | sed 's/.*://' | sort -n | uniq | wc -l || true)"
[[ "$offs" -eq 4 ]] || { cat "$work/fp_array_uniform_param_f.params" >&2; fail "expected 4 distinct relocation offsets across u_colors[2], u_scale, u_more[1], u_global; got $offs distinct"; }
printf '  %-40s ordinals 1/2/3/none, 4 distinct offsets\n' "FP parameter arrays + later params"

# ---------------------------------------------------------------------------
# VERTEX: a used element takes a constant register; unused ones are declared
# with none.  Registers DESCEND from c467 in ASCENDING element order, which
# the two-elements row pins with used elements that are NOT the first ones.
accept vp_array_uniform_const_v
params vp_array_uniform_const_v > "$work/vp_array_uniform_const_v.params"
element_order vp_array_uniform_const_v u_bones 4
expect_record vp_array_uniform_const_v 'u_bones\[2\] type=1048 paramno=none ref=1 resIndex=0x1d3 embedded=0'
expect_record vp_array_uniform_const_v 'u_bones\[0\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record vp_array_uniform_const_v 'u_bones\[3\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
printf '  %-40s [2] -> c467, others unassigned\n' "VP u_bones[2]"

accept vp_array_uniform_two_elements_v
params vp_array_uniform_two_elements_v > "$work/vp_array_uniform_two_elements_v.params"
expect_record vp_array_uniform_two_elements_v 'u_bones\[1\] type=1048 paramno=none ref=1 resIndex=0x1d3 embedded=0'
expect_record vp_array_uniform_two_elements_v 'u_bones\[3\] type=1048 paramno=none ref=1 resIndex=0x1d2 embedded=0'
expect_record vp_array_uniform_two_elements_v 'u_bones\[0\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
expect_record vp_array_uniform_two_elements_v 'u_bones\[2\] type=1048 paramno=none ref=0 resIndex=none embedded=0'
printf '  %-40s [1] -> c467, [3] -> c466\n' "VP u_bones[3] * u_bones[1]"

# HALF elements: the reference records a `half` element as FLOAT (1045) and
# a `half4` element as FLOAT4 (1048), and lays them out exactly as float
# elements; the SDK's blur family is `half gWeight[8]`.
accept fp_array_uniform_half_f
params fp_array_uniform_half_f > "$work/fp_array_uniform_half_f.params"
element_order fp_array_uniform_half_f u_w 3
expect_record fp_array_uniform_half_f 'u_w\[0\] type=1045 paramno=none ref=0 resIndex=none embedded=0'
expect_record fp_array_uniform_half_f 'u_w\[1\] type=1045 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
expect_record fp_array_uniform_half_f 'u_w\[2\] type=1045 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
accept fp_array_uniform_half4_f
params fp_array_uniform_half4_f > "$work/fp_array_uniform_half4_f.params"
expect_record fp_array_uniform_half4_f 'u_h\[1\] type=1048 paramno=none ref=1 resIndex=none embedded=1:[0-9]+'
accept vp_array_uniform_half4_v
params vp_array_uniform_half4_v > "$work/vp_array_uniform_half4_v.params"
expect_record vp_array_uniform_half4_v 'u_hb\[2\] type=1048 paramno=none ref=1 resIndex=0x1d3 embedded=0'
printf '  %-40s half -> 1045, half4 -> 1048\n' "half element arrays"

# The local-array converter is shared with uniforms: a local float4 t[2]
# must still compile (its bytes are compared to the parent by the census).
accept fp_local_vector_array_f
printf '  %-40s compiles\n' "local float4 t[2] regression"

# ---------------------------------------------------------------------------
# REFUSALS: exit exactly 1, no artifact, the body.  One checker for the
# rows and for the self-check stubs.
check_refusal() {  # <compiler> <stem> <body> -> problem or nothing
    local cc="$1" stem="$2" body="$3" rc=0
    rm -f "$work/$stem.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$cc" \
            -p "$(profile_of "$stem")" --emit-container "$work/$stem.bin" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    if [[ "$rc" -ne 1 ]]; then printf 'exited %s, expected exactly 1' "$rc"; return 0; fi
    if [[ -e "$work/$stem.bin" ]]; then printf 'refused but left an output artifact'; return 0; fi
    if ! grep -qF -- "$body" "$work/$stem.log"; then printf "stderr does not contain '%s'" "$body"; return 0; fi
}
refuse() {  # <label> <stem> <body>
    local problem; problem="$(check_refusal "$compiler" "$2" "$3")"
    [[ -z "$problem" ]] || { head -n 4 "$work/$2.log" >&2; fail "$1: $problem"; }
    printf '  %-40s refused\n' "$1"
}
refuse "FP run-time index"        fp_array_uniform_dynamic_refuse_f      "indexed at run time"
# VP run-time indices (vp_array_uniform_{dynamic,mixed,consecutive,two_arrays}_v)
# were refusal rows here until t_99b29225; they are accept rows in
# vp-array-dynamic-test.sh now.
refuse "index 4 of [4]"           fp_array_uniform_oob_refuse_f          "array index 4 out of bounds"
refuse "index -1"                 fp_array_uniform_negative_refuse_f     "array index -1 out of bounds"
refuse "index 4 / 0"              fp_array_uniform_invalid_const_refuse_f "divides by zero"
# (int)(bool)(float)0.5 is element 1 to the reference (the fraction is
# truthy); this evaluator cannot carry the fraction through the casts and
# refuses by name rather than reading element 0 (review: codex; flips to an
# accept row when the typed evaluator serves the index path).
refuse "(int)(bool)(float)0.5 (reference: element 1)" fp_array_uniform_cast_bool_float_f "floating value"
refuse "(fixed)3 (reference: element 1; clamp is the typed evaluator's)" fp_array_uniform_fixed_cast_refuse_f "floating value"
refuse "float4x4 element array"   fp_array_uniform_matrix_refuse_f       "element type this lowering does not lay out"

# SELF-CHECK: an exit-1 stub that leaves an EMPTY artifact, and one that
# refuses for an unrelated reason, must both be rejected by the checker.
stub="$work/stub"
{ echo '#!/usr/bin/env bash'; echo 'for a in "$@"; do case "$prev" in --emit-container) : > "$a";; esac; prev="$a"; done'; echo 'echo "x:1:1: error: indexed at run time" >&2'; echo 'exit 1'; } > "$stub"; chmod +x "$stub"
problem="$(check_refusal "$stub" fp_array_uniform_dynamic_refuse_f "indexed at run time")"
[[ "$problem" == *"left an output artifact"* ]] || fail "self-check: an empty-artifact stub was not rejected ('$problem')"
{ echo '#!/usr/bin/env bash'; echo 'echo "x:1:1: error: unrelated stage failure" >&2'; echo 'exit 1'; } > "$stub"
problem="$(check_refusal "$stub" fp_array_uniform_dynamic_refuse_f "indexed at run time")"
[[ "$problem" == *"does not contain"* ]] || fail "self-check: an unrelated-refusal stub was not rejected ('$problem')"
printf '  %-40s ok\n' "self-check: two stubs rejected"

printf 'PASS: array-uniform-test\n'
