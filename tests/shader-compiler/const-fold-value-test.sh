#!/usr/bin/env bash
# A file-scope `const` must reach the ucode as its initialiser (file-scope-const-initializer).
#
# The defect this pins: the initialiser was parsed and dropped, and each
# backend then invented a different wrong value -- fragment default compiled
# K as 0.0, fragment general read it as vertex attribute 0, vertex refused
# with "LoadUniform for unknown global 'K'".
#
# A NON-static file-scope const is a patchable uniform on the reference
# (t_528b9869): record K, float, referenced, default 7.5, read from an inline
# constant holding 7.5 - so it is NOT byte-equal to the inline literal.  What
# must hold is the value: the program computes what the literal does.
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

for path in general; do
    compile fp_file_scope_const_f   "const_$path"
    compile fp_inline_literal_f     "lit_$path"
    compile fp_inline_literal_alt_f "alt_$path"

    # The const and the inline literal compute the same value.
    python3 - "$repo_root/tests/shader-compiler" "$work/const_$path.fpo" "$work/lit_$path.fpo" <<'PY' || fail "file-scope const and the inline literal compute different values on the $path path"
import sys
sys.path.insert(0, sys.argv[1])
import fp_eval
a, b = (open(p, 'rb').read() for p in sys.argv[2:4])
for uv in ([0.25, 0.5, 0, 0], [-1.5, 2.0, 0, 0]):
    x, y = fp_eval.evaluate(a, {'TEX0': uv}), fp_eval.evaluate(b, {'TEX0': uv})
    if x != y:
        raise SystemExit('const %s vs literal %s at %s' % (x, y, uv))
PY

    # Negative control: a different constant must compute a different value.
    # Without this, an evaluator that ignored the constant would pass above.
    python3 - "$repo_root/tests/shader-compiler" "$work/const_$path.fpo" "$work/alt_$path.fpo" <<'PY' || fail "a different constant computed the same value on the $path path - the check above cannot tell values apart"
import sys
sys.path.insert(0, sys.argv[1])
import fp_eval
a, b = (open(p, 'rb').read() for p in sys.argv[2:4])
if fp_eval.evaluate(a, {'TEX0': [0.25, 0.5, 0, 0]}) == fp_eval.evaluate(b, {'TEX0': [0.25, 0.5, 0, 0]}):
    raise SystemExit(1)
PY
done

# The record, as measured on the reference: K, float (1045), referenced,
# default 7.5 in its raw 16-byte block.
python3 - "$work/const_general.fpo" <<'PY'
import struct, sys
blob = open(sys.argv[1], 'rb').read()
count, table = struct.unpack_from('>2I', blob, 12)
records = {}
for i in range(count):
    w = struct.unpack_from('>12I', blob, table + 48 * i)
    records[blob[w[4]:blob.index(0, w[4])].decode()] = w
k = records.get('K')
if k is None:
    raise SystemExit('FAIL: no record for the non-static const K')
default = blob[k[5]:k[5] + 16] if k[5] else None
if (k[0], k[10], default) != (1045, 1, struct.pack('>4f', 7.5, 0, 0, 0)):
    raise SystemExit('FAIL: record K is type %d ref %d default %r; want 1045, 1, 7.5' % (k[0], k[10], default))
PY

# The value itself is in the ucode, not merely agreement between two files.
# 7.5f is 0x40F00000, stored halfword-swapped in the ucode blob as 0000 40F0.
python3 - "$work/const_general.fpo" <<'PY'
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
