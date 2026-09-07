#!/usr/bin/env bash
# A CAST FROM A SCALAR TO A VECTOR IS A BROADCAST.
#
# `(float4)d` and `(half4)d` are the cast spelling of `float4(d,d,d,d)`, and
# the reference lowers all three to ONE instruction (sce-cgc 475, measured
# 2026-09-07):
#
#   float d = a.x; return (float4)d;        MOV R0.xyzw, TEX0.xxxx
#   half  d = a.x; return (half4)d;         MOV H0.xyzw, TEX0.xxxx  prec=1
#   half  d = a.x; return half4(d,d,d,d);   MOV H0.xyzw, TEX0.xxxx  prec=1
#
# Only the constructor reached VecConstruct in the builder; the cast fell
# through to the default IROp::Bitcast, and the NV40 general path refuses
# that outright - "unsupported IR op bitcast".  It is 22 rows of the
# reference-SDK sweep and it is NOT half-only: the float spelling refused
# too, which is why both are pinned here (t_cde25bad).
#
# The assertion is the TWIN, not a shape: the cast container must be
# byte-identical to the constructor container.  A fix that lowers the cast to
# something merely valid - a different register, an extra move, the wrong
# precision - fails here even though it compiles.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-cast-splat.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
decoder="$repo_root/tests/shader-compiler/fp_sources.py"

emit() {   # <stem>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    [[ "$rc" -ne 124 ]] || fail "$stem timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$stem.err" >&2
        fail "$stem did not compile.  A scalar-to-vector CAST is a broadcast;
refusing it as an unsupported bitcast is the defect this test exists for."
    fi
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
}

for stem in fp_cast_splat_float_f fp_cast_splat_float_ctor_f \
            fp_cast_splat_half_f fp_cast_splat_half_ctor_f; do
    emit "$stem"
done

twin() {   # <cast stem> <constructor stem> <what>
    if ! cmp -s "$work/$1.fpo" "$work/$2.fpo"; then
        python3 "$decoder" "$work/$1.fpo" | sed 's/^/  cast: /' >&2
        python3 "$decoder" "$work/$2.fpo" | sed 's/^/  ctor: /' >&2
        fail "$3: the cast spelling and the constructor spelling produced
different containers.  They are the same program - the reference emits one
MOV with a broadcast swizzle for both."
    fi
}

twin fp_cast_splat_float_f fp_cast_splat_float_ctor_f "float scalar to float4"
twin fp_cast_splat_half_f  fp_cast_splat_half_ctor_f  "half scalar to half4"

# The half spelling must also still be HALF: a broadcast that lost the
# precision would pass the twin test above while rendering at fp32.
python3 - "$work/fp_cast_splat_half_f.fpo" "$decoder" <<'PY'
import re
import subprocess
import sys

rows = subprocess.run([sys.executable, sys.argv[2], sys.argv[1]],
                      capture_output=True, text=True, check=True).stdout
moves = [l for l in rows.splitlines() if re.match(r"^\d+ MOV ", l)]
if not moves:
    raise SystemExit("FAIL: the half broadcast emitted no MOV at all: %r" % rows)
if not any("dst=H" in l and "prec=1" in l for l in moves):
    raise SystemExit(
        "FAIL: the half broadcast is not half - no MOV writes an H register "
        "at prec=1.  The reference emits MOV H0.xyzw, TEX0.xxxx at prec=1.\n%s"
        % rows)
print("cast-splat: half broadcast writes H at prec=1")
PY

printf 'cast-splat: PASS\n'
