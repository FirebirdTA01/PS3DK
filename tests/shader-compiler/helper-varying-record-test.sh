#!/usr/bin/env bash
# A STRUCT INPUT MEMBER READ ONLY THROUGH A HELPER KEEPS ITS VARYING RECORD, REFERENCED.
#
# `struct Tag { float2 uv : TEXCOORD0; float3 col : TEXCOORD1; };
#  float3 helper(Tag t) { return t.col; }
#  float4 main(Tag i) : COLOR { return float4(helper(i) * i.uv.x, 1); }`
# compiled to the same ucode as reading i.col directly, but the i.col record
# was written UNREFERENCED (one byte, the record's referenced flag): the load
# built inside the inlined helper was stamped with the helper's parameter
# name, and the container writer marks a record referenced only from loads
# stamped with an ENTRY parameter's name.  Silent: the program ran; anything
# binding inputs by record saw the input as unused.
#
# The IR builder now stamps a load made inside an inlined helper with the
# entry parameter the helper's parameter is bound to.  Every expectation
# below is measured on the reference (sce-cgc 475 -p sce_fp_rsx, 2026-10-16):
# each helper spelling is byte-identical to its direct twin there, and the
# record tables are the reference's own.
#   - one helper, two helpers, an unsemanticked (inferred-slot) member;
#   - unused member: i.n is never read and stays unreferenced;
#   - two entry inputs: the helper is given b, so b.col is referenced and
#     a.col is not - distinct struct types, and one struct type for both.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-helper-varying-record.XXXXXX")"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

compile() {
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    rm -f "$work/$stem.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.bin" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -ne 124 ]] || fail "$stem timed out"
    [[ "$rc" -lt 128 ]] || fail "$stem crashed"
    if [[ "$rc" -ne 0 ]]; then
        head -n 4 "$work/$stem.log" >&2
        fail "$stem did not compile; the reference accepts it"
    fi
    [[ -s "$work/$stem.bin" ]] || fail "$stem wrote no container"
}

# records <stem> <expected>: the container's (name resource referenced) table,
# in order, must be exactly <expected> ("name:resource:referenced ...").
records() {
    local stem="$1" expected="$2" got rc=0
    got="$(python3 - "$work/$stem.bin" "$repo_root/tests/shader-compiler" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from uniform_container_check import Container
c = Container(open(sys.argv[1], "rb").read())
print(" ".join("%s:%d:%d" % (r["name"], r["resource"], r["referenced"]) for r in c.records))
PY
)" || rc=$?
    [[ "$rc" -eq 0 ]] || fail "$stem: the container did not decode (python exit $rc)"
    [[ "$got" == "$expected" ]] \
        || fail "$stem records differ from the reference"$'\n'"  got:      $got"$'\n'"  expected: $expected"
}

twin() {
    compile "$1"; compile "$2"
    cmp -s "$work/$1.bin" "$work/$2.bin" \
        || fail "$3: the helper spelling and the direct read produced different containers ($(stat -c %s "$work/$1.bin") vs $(stat -c %s "$work/$2.bin") bytes); the reference emits the same bytes"
    records "$1" "$4"
    printf 'PASS %s\n' "$3"
}

twin fp_helper_record_f       fp_helper_record_ctl_f "i.col read through one helper" \
    "i.uv:3220:1 i.col:3221:1 main:2757:1"
twin fp_helper_record_chain_f fp_helper_record_ctl_f "i.col read through two helpers" \
    "i.uv:3220:1 i.col:3221:1 main:2757:1"
twin fp_helper_record_unsem_f fp_helper_record_unsem_ctl_f "i.col unsemanticked, read through a helper" \
    "i.uv:3220:1 i.col:3221:1 main:2757:1"
twin fp_helper_record_dead_f  fp_helper_record_dead_ctl_f "unused member i.n stays unreferenced" \
    "i.uv:3220:1 i.col:3221:1 i.n:3222:0 main:2757:1"
twin fp_helper_record_two_f   fp_helper_record_two_ctl_f "two entry inputs: the helper's b.col, not a.col" \
    "a.uv:3220:1 a.col:3221:0 b.uv:3222:0 b.col:3223:1 main:2757:1"
twin fp_helper_record_same_f  fp_helper_record_same_ctl_f "two entry inputs of one type: b.col, not a.col" \
    "a.uv:3220:1 a.col:3256:0 b.uv:3256:0 b.col:3221:1 main:2757:1"

printf 'helper-varying-record-test: PASS\n'
