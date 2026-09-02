#!/usr/bin/env bash
# Work that FOLLOWS a discard must reach the ucode (t_72810bd7).
#
# THE ASSERTION FLIPPED on 2026-09-02, as the item said it would.  While
# the post-discard lerp was not a shape this path lowered, the required
# behaviour was a REFUSAL - compiling it meant shipping a shader missing a
# line of its source, which is what th06_notex did for months.  The lerp
# lowers now, so the requirement is the stronger one: the shader compiles
# AND the work is there.
#
# "The work is there" is asserted on the UCODE, not on the container's
# input mask.  The mask is not evidence: t_e89cd261 was a defect where the
# mask named a varying no instruction read, so a shader can claim an input
# it never touches.  The lerp reads TEXCOORD2, so some instruction must
# name input source 6 - and if the lerp were dropped again, only the
# colour's TEXCOORD0 would appear.
#
# Two controls, because this fails in both directions: it can miss a drop,
# or it can refuse the whole discard class.  The negative control is a real
# discard shader from the samples.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-discard-completeness-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run() {   # $1 shader path, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$2.fpo" "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    return "$rc"
}

# POSITIVE: post-discard work must compile AND be present in the ucode.
pos="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_discard_then_work_f.cg"
if ! run "$pos" positive; then
    tail -n 10 "$work/positive.log" >&2
    fail "fp_discard_then_work did NOT compile. Its post-discard lerp is a
shape this path lowers now; refusing it is a regression to the behaviour
t_72810bd7 replaced."
fi

# The container run above prints no ucode, so take the dump separately.
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" -p sce_fp_rsx "$pos"
) >"$work/positive-dump.log" 2>&1 ||
    fail "fp_discard_then_work compiled with --emit-container but not without it"

python3 - "$work/positive-dump.log" <<'PY'
import re
import sys

# NV40 fragment INPUT_SRC selector: TEXCOORDn is 4 + n, in word0 bits 13..16.
TEX0, TEX2 = 4, 6
REG_TYPE_INPUT = 1


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


read = set()
for line in open(sys.argv[1], "r", encoding="utf-8"):
    m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
    if not m:
        continue
    w = [unswap(int(x, 16)) for x in m.group(2).split()]
    if len(w) < 4:
        continue
    opcode = (w[0] >> 24) & 0x3F
    if opcode == 0 or opcode > 0x40:
        continue
    if any((w[i] & 3) == REG_TYPE_INPUT for i in (1, 2, 3)):
        read.add((w[0] >> 13) & 0xF)

if not read:
    raise SystemExit(
        "FAIL: no instruction reads a varying at all - the ucode dump did "
        "not parse, so nothing below was actually checked"
    )
if TEX2 not in read:
    raise SystemExit(
        "FAIL: no instruction reads TEXCOORD2 (input source %d); the ucode "
        "names %s.  The fog lerp after the discard reads it, so the work was "
        "dropped - which is the whole of t_72810bd7.  Asserted on the ucode "
        "and not on attributeInputMask, because a container can name an "
        "input no instruction touches (t_e89cd261)."
        % (TEX2, sorted(read))
    )
if TEX0 not in read:
    raise SystemExit(
        "FAIL: no instruction reads TEXCOORD0 (input source %d); the ucode "
        "names %s.  The colour the lerp blends comes from it." % (TEX0, sorted(read))
    )
PY

# NEGATIVE: a discard shader this path lowers correctly -> must still compile.
neg="$repo_root/samples/gcm/hello-ppu-cellgcm-discard-blend/shaders/fpshader.fcg"
if [[ -f "$neg" ]]; then
    if ! run "$neg" negative; then
        tail -n 10 "$work/negative.log" >&2
        fail "the discard-blend sample no longer compiles: the guard is
refusing the whole discard class rather than the dropped-work case"
    fi
else
    fail "negative control shader is missing: $neg"
fi

printf 'discard-completeness-test: ok\n'
