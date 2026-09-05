#!/usr/bin/env bash
# A fragment entry's `out` parameter with no semantic binds to COLOR
# (t_a15ec129).  The store used to be gated on the parameter HAVING a
# semantic, so a shader declared this way emitted no StoreOutput at all -
# and everything that only fed the output went with it.  Exit 0, no
# diagnostic, and a container whose ucode never writes the output register.
#
# The assertion is the twin: the same shader with `: COLOR` written out
# must compile to the same PROGRAM, on both paths.  It cannot pass by
# accident - if the omitted semantic changed what the shader computes, the
# two would differ.
#
# UCODE, not the whole container, and that limit is the reference's own.
# It records the DECLARED semantic beside the resource - "out.UNDEFINED:
# COLOR0" without the semantic, "out.COLOR: COLOR0" with it - so the two
# containers legitimately differ by that string, and ours are byte-identical
# to the reference's respective containers.  An identity check on the whole
# container would be stronger than the oracle, which is a mistake worth not
# making twice.
#
# It also checks the output register is actually written, so the pair
# cannot both be empty and agree.  Blank-vs-blank is not identical.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-out-no-semantic-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
for f in fp_out_no_semantic_f.cg fp_out_with_semantic_f.cg; do
    [[ -f "$shaders/$f" ]] || fail "fixture missing: $shaders/$f"
done

compile() {   # $1 stem, $2 flags, $3 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${2:+$2} --emit-container "$work/$3.fpo" \
            "$shaders/$1.cg"
    ) >"$work/$3.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$3 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$3.log" >&2
        fail "$3 did not compile.  A fragment out parameter with no semantic
binds to COLOR; refusing or dropping it is t_a15ec129."
    fi
    # A separate run for the ucode: --emit-container suppresses the dump.
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${2:+$2} "$shaders/$1.cg"
    ) >"$work/$3.dump" 2>&1 || fail "$3 compiled with a container but not without"
}

# Shelf-life: when the retired legacy matcher is removed, drop this second
# --legacy-lowering run and its header claim in the same commit.
for path_flags in ":general" "--legacy-lowering:legacy"; do
    flags="${path_flags%%:*}"
    tag="${path_flags##*:}"
    compile fp_out_no_semantic_f   "$flags" "none_$tag"
    compile fp_out_with_semantic_f "$flags" "sem_$tag"

    # The ucode rows only: the parameter table legitimately differs by the
    # declared semantic string, exactly as the reference's does.
    grep -E '^ +[0-9]+:' "$work/none_$tag.dump" > "$work/none_$tag.ucode" || true
    grep -E '^ +[0-9]+:' "$work/sem_$tag.dump"  > "$work/sem_$tag.ucode"  || true
    if ! cmp -s "$work/none_$tag.ucode" "$work/sem_$tag.ucode"; then
        diff "$work/none_$tag.ucode" "$work/sem_$tag.ucode" >&2 || true
        fail "on the $tag path the ucode for an out parameter WITHOUT a
semantic differs from the one WITH ': COLOR'.  They are the same program:
the reference binds an unsemanticked fragment out to COLOR0, and so must we
(t_a15ec129)."
    fi
done

python3 - "$work"/none_*.dump <<'PY'
import re
import sys


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


for path in sys.argv[1:]:
    insns = 0
    writes_output = 0
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if not m:
            continue
        w = [unswap(int(x, 16)) for x in m.group(2).split()]
        if len(w) < 4:
            continue
        opcode = (w[0] >> 24) & 0x3F
        if opcode == 0 or opcode > 0x40:
            continue
        insns += 1
        # The colour output is register index 0 on this hardware.
        if ((w[0] >> 1) & 0x3F) == 0:
            writes_output += 1
    name = path.replace("\\", "/").split("/")[-1]
    if insns == 0:
        raise SystemExit(
            "FAIL: %s emitted no instructions, so the identity check above "
            "compared two empty containers - blank against blank is not "
            "identical" % name
        )
    if writes_output == 0:
        raise SystemExit(
            "FAIL: %s never writes the output register.  That is the whole "
            "defect: the store was dropped and everything feeding it went "
            "with it, leaving a well-formed container that paints whatever "
            "R0 held (t_a15ec129)." % name
        )
PY

printf 'out-no-semantic-test: ok (both paths, ucode identity + output written)\n'
