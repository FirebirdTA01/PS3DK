#!/usr/bin/env bash
# A literal operand of a fused multiply-add carries its value into the
# ucode (t_a1f43b12).  Before the fix the emitter treated whichever
# operand was not the varying as a uniform and wrote a zero placeholder
# for the host to patch; nothing patched it, so `v * 0.5 + 0.5` shipped as
# `v * 0 + 0.5` and painted a flat colour.  Exit 0, no diagnostic.
#
# The assertions are on the emitted ucode, since the exit status was never
# the problem.  The retired matcher is pinned to the reference compiler's
# compact shape; the shipping general path is pinned to the visible property
# that the literal multiplier reaches the container as 0.5 instead of as an
# unpatched zero placeholder.
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

compile "$both" both_legacy --legacy-lowering
compile "$mixed" mixed_legacy --legacy-lowering
compile "$both" both_general
compile "$mixed" mixed_general

python3 - "$work/both_legacy.log" "$work/mixed_legacy.log" \
        "$work/both_general.log" "$work/mixed_general.log" <<'PY'
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
CONST_XXXX = 0x00020000  # a source reading c[0] with an .xxxx swizzle
ZEROS = [0, 0, 0, 0]
HALF4 = [HALF, HALF, HALF, HALF]

# --- both operands literal -------------------------------------------
# MOVR R0, f[TEX0] ; MADR R0, R0, c.xxxx, c.xxxx ; {0.5, 0, 0, 0}
both_legacy = rows(sys.argv[1])
if len(both_legacy) != 3:
    raise SystemExit(
        "FAIL: fp_mad_literal_f must emit the varying MOV, one MAD and one "
        "shared const block - three ucode rows, not %d.  Six rows is the "
        "pre-fix preload shape, whose R1 held the zero placeholder nothing "
        "ever patched (t_a1f43b12)." % len(both_legacy)
    )
if both_legacy[2] != [HALF, 0, 0, 0]:
    raise SystemExit(
        "FAIL: the MAD's shared const block must hold 0.5 once, packed and "
        "zero-filled; got [%s]"
        % ", ".join("0x%08x" % w for w in both_legacy[2])
    )
if both_legacy[1][2] != CONST_XXXX or both_legacy[1][3] != CONST_XXXX:
    raise SystemExit(
        "FAIL: both MAD operands must read the shared const block with an "
        ".xxxx swizzle (0x%08x); got SRC1 0x%08x SRC2 0x%08x.  A multiplier "
        "that does not read the block is the defect: it multiplied by the "
        "zero placeholder."
        % (CONST_XXXX, both_legacy[1][2], both_legacy[1][3])
    )

# --- literal multiplier, uniform addend -------------------------------
# MOVR R0 ; MOVR R1, k + zero block ; FENCBR ; MADR R0, R0, c.xxxx, R1 + {0.5}
mixed_legacy = rows(sys.argv[2])
if len(mixed_legacy) != 6:
    raise SystemExit(
        "FAIL: fp_mad_literal_uniform_f must emit six ucode rows (two MOVs "
        "with their blocks, the fence and the MAD with its block), not %d"
        % len(mixed_legacy)
    )
if mixed_legacy[2] != ZEROS:
    raise SystemExit(
        "FAIL: the preloaded UNIFORM's block must stay zero for the host to "
        "patch; got [%s]"
        % ", ".join("0x%08x" % w for w in mixed_legacy[2])
    )
if mixed_legacy[5] != [HALF, 0, 0, 0]:
    raise SystemExit(
        "FAIL: the literal multiplier must reach the MAD's own const block "
        "as 0.5; got [%s].  All zeros there is the defect - the output "
        "becomes the uniform addend alone."
        % ", ".join("0x%08x" % w for w in mixed_legacy[5])
    )
if mixed_legacy[4][2] != CONST_XXXX:
    raise SystemExit(
        "FAIL: the MAD's multiplier operand must read the const block with "
        "an .xxxx swizzle (0x%08x); got 0x%08x"
        % (CONST_XXXX, mixed_legacy[4][2])
    )


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
            if i + 1 < len(rs) and rs[i + 1] == HALF4:
                return True
    return False


def mad_multiplier_is_half(rs, label):
    for i in instruction_indices(rs):
        if opcode(rs[i]) != 0x04:
            continue
        # MAD source 1 is the multiplier.  It may be an inline literal const
        # or a temp preloaded from that literal block, depending on whether
        # the addend also needs an inline const/uniform source.
        if src_type(rs[i], 1) == 2 and i + 1 < len(rs) and rs[i + 1] == HALF4:
            return
        if src_type(rs[i], 1) == 0 and temp_written_from_half(
                rs, i, src_reg(rs[i], 1)):
            return
    raise SystemExit(
        "FAIL: general %s MAD multiplier must read a 0.5 literal block, "
        "either inline or through a temp preloaded from that block.  If it "
        "reads the zero uniform patch block instead, the shader compiles "
        "and paints a flat colour (t_a1f43b12)." % label
    )


# Shipping general lowering preloads literals into temps.  It is not byte-
# shaped like the reference, but it must still carry the 0.5 multiplier as
# data.  If that block vanishes or becomes zero, the pixels collapse to the
# old flat-colour t_a1f43b12 failure.
both_general = rows(sys.argv[3])
blocks = const_blocks(both_general)
if HALF4 not in blocks:
    raise SystemExit(
        "FAIL: general fp_mad_literal_f must carry a 0.5 literal block for "
        "the MAD multiplier; blocks were [%s].  Without that block the "
        "shipping path multiplies by zero and paints a flat colour "
        "(t_a1f43b12)."
        % "; ".join(",".join("0x%08x" % w for w in b) for b in blocks)
    )
if ZEROS in blocks:
    raise SystemExit(
        "FAIL: general fp_mad_literal_f has no uniform input, so an all-zero "
        "patch block means the literal multiplier can be treated as an "
        "unpatched uniform (t_a1f43b12)."
    )
mad_multiplier_is_half(both_general, "fp_mad_literal_f")

mixed_general = rows(sys.argv[4])
blocks = const_blocks(mixed_general)
if ZEROS not in blocks or HALF4 not in blocks:
    raise SystemExit(
        "FAIL: general fp_mad_literal_uniform_f must carry both the zero "
        "uniform patch block and the 0.5 literal multiplier block; blocks "
        "were [%s].  Missing the literal block is the silent flat-colour "
        "failure (t_a1f43b12)."
        % "; ".join(",".join("0x%08x" % w for w in b) for b in blocks)
    )
mad_multiplier_is_half(mixed_general, "fp_mad_literal_uniform_f")
PY

printf 'mad-literal-operand-test: ok\n'
