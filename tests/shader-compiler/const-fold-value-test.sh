#!/usr/bin/env bash
# A file-scope `const` must reach the ucode as its initialiser (t_4584aa27).
#
# The defect this pins: the initialiser was parsed and dropped, and each
# backend then invented a different wrong value -- fragment default compiled
# K as 0.0, fragment general read it as vertex attribute 0, vertex refused
# with "LoadUniform for unknown global 'K'".  Byte-equality against an inline
# literal is checked on BOTH lowering paths, because the two paths failed
# differently and a fix that only corrected one would pass a single-path test.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
work="${TMPDIR:-/tmp}/ps3dk-shader-const-fold-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# $1 shader stem, $2 output tag, $3.. extra flags
compile() {
    local stem="$1" tag="$2"
    shift 2
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$@" \
            --emit-container "$work/$tag.fpo" "$shaders/$stem.cg"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$stem ($tag) timed out"
    [[ "$rc" -eq 134 || "$rc" -eq 137 ]] && fail "$stem ($tag) aborted under memory cap"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$tag.log" >&2
        fail "$stem ($tag) failed to compile"
    fi
    [[ -s "$work/$tag.fpo" ]] || fail "$stem ($tag) produced no container"
}

# Shelf-life: when the retired legacy matcher is removed, drop this second
# --legacy-lowering run and its header claim in the same commit.
for path in general legacy; do
    flags=()
    [[ "$path" == legacy ]] && flags=(--legacy-lowering)

    compile fp_file_scope_const_f   "const_$path"  "${flags[@]}"
    compile fp_inline_literal_f     "lit_$path"    "${flags[@]}"
    compile fp_inline_literal_alt_f "alt_$path"    "${flags[@]}"

    # The fold carries the value: same value, same bytes.
    if ! cmp -s "$work/const_$path.fpo" "$work/lit_$path.fpo"; then
        printf 'first differing bytes (%s path):\n' "$path" >&2
        cmp -l "$work/const_$path.fpo" "$work/lit_$path.fpo" | head -n 8 >&2
        fail "file-scope const does not compile to the same container as the" \
             "inline literal on the $path path"
    fi

    # Negative control: a different value must NOT produce the same container.
    # Without this, a compiler that dropped the multiply would pass above.
    if cmp -s "$work/const_$path.fpo" "$work/alt_$path.fpo"; then
        fail "a different constant produced an identical container on the" \
             "$path path - the test cannot tell values apart, so the equality" \
             "above proves nothing"
    fi
done

# The value itself is in the ucode, not merely agreement between two files.
# 7.5f is 0x40F00000, stored halfword-swapped in the ucode blob as 0000 40F0.
python3 - "$work/const_general.fpo" "$work/const_legacy.fpo" <<'PY'
import sys

needle = bytes((0x00, 0x00, 0x40, 0xF0))   # 7.5f, halfword-swapped
absent = bytes((0x00, 0x00, 0x00, 0x00))   # what the defect emitted instead

for path in sys.argv[1:]:
    blob = open(path, "rb").read()
    if needle not in blob:
        raise SystemExit(
            "FAIL: %s does not contain the folded initialiser 7.5 "
            "(halfword-swapped 0000 40F0); the const's value did not reach "
            "the ucode" % path
        )
    # Self-test the detector: it must be capable of reporting a miss.  A
    # needle that matched anything would make the assertion above vacuous.
    if needle == absent:
        raise SystemExit("FAIL: detector needle is the zero pattern")
PY

printf 'const-fold-value-test: ok\n'
