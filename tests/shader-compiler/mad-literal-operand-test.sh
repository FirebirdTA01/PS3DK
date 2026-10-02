#!/usr/bin/env bash
# A literal operand of a fused multiply-add carries its value into the
# ucode (mad-literal-data).  Before the fix the emitter treated whichever
# operand was not the varying as a uniform and wrote a zero placeholder
# for the host to patch; nothing patched it, so `v * 0.5 + 0.5` shipped as
# `v * 0 + 0.5` and painted a flat colour.  Exit 0, no diagnostic.
#
# The assertions are on the emitted ucode, since the exit status was never
# the problem.  The general path is pinned to the visible property that the
# literal multiplier reaches the container as 0.5 instead of as an unpatched
# zero placeholder.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-mad-literal-operand-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {   # $1 shader, $2 tag, $3 extra flags
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${3:+$3} "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 failed to compile"
    fi
}

both="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_mad_literal_f.cg"
mixed="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_mad_literal_uniform_f.cg"
[[ -f "$both" ]]  || fail "fixture missing: $both"
[[ -f "$mixed" ]] || fail "fixture missing: $mixed"

compile "$both" both_general
compile "$mixed" mixed_general

python3 - "$work/both_general.log" "$work/mixed_general.log" <<'PY'
import re
import sys


def rows(path):
    out = []
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if m:
            out.append([int(w, 16) for w in m.group(2).split()])
    return out


HALF = 0x00003F00        # 0.5f, in the byte order the container carries
ZEROS = [0, 0, 0, 0]
# The verbatim four-lane block the shipping path emitted BEFORE the dedup
# rule (literal-vector-dedup-swizzle).  Nothing asserts it any more; it stays in the filter
# below so a regression to that shape is COLLECTED and printed in the
# failure's block list instead of being silently filtered away.
HALF4 = [HALF, HALF, HALF, HALF]
# The dedup rule's shape: distinct values first, zero-filled (literal-vector-dedup-swizzle).
HALF_PACKED = [HALF, 0, 0, 0]


def const_blocks(rs):
    return [r for r in rs if r in (ZEROS, HALF4, [HALF, 0, 0, 0])]


def logical(disk_word):
    return ((disk_word >> 16) | ((disk_word & 0xFFFF) << 16)) & 0xFFFFFFFF


def logical_words(row):
    return [logical(w) for w in row]


def opcode(row):
    return (logical_words(row)[0] >> 24) & 0x3F


def dst(row):
    return (logical_words(row)[0] >> 1) & 0x3F


def src_word(row, src_index):
    return logical_words(row)[1 + src_index]


def src_type(row, src_index):
    return src_word(row, src_index) & 3


def src_reg(row, src_index):
    return (src_word(row, src_index) >> 2) & 0x3F


def const_srcs(row):
    return [w for w in logical_words(row)[1:4] if (w & 3) == 2]


def instruction_indices(rs):
    out = []
    i = 0
    while i < len(rs):
        cs = const_srcs(rs[i])
        out.append(i)
        i += 1 + (1 if cs else 0)
    return out


def temp_written_from_half(rs, before_idx, reg):
    for i in instruction_indices(rs):
        if i >= before_idx:
            break
        if opcode(rs[i]) == 0x01 and dst(rs[i]) == reg and const_srcs(rs[i]):
            if i + 1 < len(rs) and rs[i + 1] == HALF_PACKED:
                return True
    return False


def mad_multiplier_is_half(rs, label):
    for i in instruction_indices(rs):
        if opcode(rs[i]) != 0x04:
            continue
        # MAD source 1 is the multiplier.  It may be an inline literal const
        # or a temp preloaded from that literal block, depending on whether
        # the addend also needs an inline const/uniform source.
        if src_type(rs[i], 1) == 2 and i + 1 < len(rs) and rs[i + 1] == HALF_PACKED:
            return
        if src_type(rs[i], 1) == 0 and temp_written_from_half(
                rs, i, src_reg(rs[i], 1)):
            return
    raise SystemExit(
        "FAIL: general %s MAD multiplier must read a 0.5 literal block, "
        "either inline or through a temp preloaded from that block.  If it "
        "reads the zero uniform patch block instead, the shader compiles "
        "and paints a flat colour (mad-literal-data)." % label
    )


# General lowering preloads literals into temps and still emits one
# block per literal source where the reference shares a single block between
# the MAD's two operands - that gap is mad-literal-block-sharing's fp_mad_literal_f row and
# it is not this assertion's subject.  What IS asserted is that the 0.5
# multiplier is carried AS DATA: if that block vanishes or becomes zero, the
# pixels collapse to the old flat-colour mad-literal-data failure.
#
# THE PACKED SHAPE, not the verbatim one: the block is {0.5, 0, 0, 0} read
# .xxxx, which is what the reference emits for this fixture (see the
# fixture's own header).  It
# used to be {0.5, 0.5, 0.5, 0.5} here, because the shipping path wrote
# every lane verbatim; literal-vector-dedup-swizzle's dedup landed and this expectation moved
# with it rather than pinning the shape the compiler no longer has.
both_general = rows(sys.argv[1])
blocks = const_blocks(both_general)
if HALF_PACKED not in blocks:
    raise SystemExit(
        "FAIL: general fp_mad_literal_f must carry a 0.5 literal block for "
        "the MAD multiplier, packed as {0.5, 0, 0, 0}; blocks were [%s].  "
        "Without that block the shipping path multiplies by zero and paints "
        "a flat colour (mad-literal-data)."
        % "; ".join(",".join("0x%08x" % w for w in b) for b in blocks)
    )
if ZEROS in blocks:
    raise SystemExit(
        "FAIL: general fp_mad_literal_f has no uniform input, so an all-zero "
        "patch block means the literal multiplier can be treated as an "
        "unpatched uniform (mad-literal-data)."
    )
mad_multiplier_is_half(both_general, "fp_mad_literal_f")

mixed_general = rows(sys.argv[2])
blocks = const_blocks(mixed_general)
if ZEROS not in blocks or HALF_PACKED not in blocks:
    raise SystemExit(
        "FAIL: general fp_mad_literal_uniform_f must carry both the zero "
        "uniform patch block and the 0.5 literal multiplier block; blocks "
        "were [%s].  Missing the literal block is the silent flat-colour "
        "failure (mad-literal-data)."
        % "; ".join(",".join("0x%08x" % w for w in b) for b in blocks)
    )
mad_multiplier_is_half(mixed_general, "fp_mad_literal_uniform_f")
PY

printf 'mad-literal-operand-test: ok\n'
