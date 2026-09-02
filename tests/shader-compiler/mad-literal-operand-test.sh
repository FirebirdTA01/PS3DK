#!/usr/bin/env bash
# A literal operand of a fused multiply-add carries its value into the
# ucode (t_a1f43b12).  Before the fix the emitter treated whichever
# operand was not the varying as a uniform and wrote a zero placeholder
# for the host to patch; nothing patched it, so `v * 0.5 + 0.5` shipped as
# `v * 0 + 0.5` and painted a flat colour.  Exit 0, no diagnostic.
#
# The assertions are on the emitted ucode, since the exit status was never
# the problem, and they are the reference compiler's own shapes: our
# containers are byte-identical to it for both fixtures.
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

compile() {   # $1 shader, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --legacy-lowering "$1"
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

compile "$both" both
compile "$mixed" mixed

python3 - "$work/both.log" "$work/mixed.log" <<'PY'
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

# --- both operands literal -------------------------------------------
# MOVR R0, f[TEX0] ; MADR R0, R0, c.xxxx, c.xxxx ; {0.5, 0, 0, 0}
both = rows(sys.argv[1])
if len(both) != 3:
    raise SystemExit(
        "FAIL: fp_mad_literal_f must emit the varying MOV, one MAD and one "
        "shared const block - three ucode rows, not %d.  Six rows is the "
        "pre-fix preload shape, whose R1 held the zero placeholder nothing "
        "ever patched (t_a1f43b12)." % len(both)
    )
if both[2] != [HALF, 0, 0, 0]:
    raise SystemExit(
        "FAIL: the MAD's shared const block must hold 0.5 once, packed and "
        "zero-filled; got [%s]" % ", ".join("0x%08x" % w for w in both[2])
    )
if both[1][2] != CONST_XXXX or both[1][3] != CONST_XXXX:
    raise SystemExit(
        "FAIL: both MAD operands must read the shared const block with an "
        ".xxxx swizzle (0x%08x); got SRC1 0x%08x SRC2 0x%08x.  A multiplier "
        "that does not read the block is the defect: it multiplied by the "
        "zero placeholder." % (CONST_XXXX, both[1][2], both[1][3])
    )

# --- literal multiplier, uniform addend -------------------------------
# MOVR R0 ; MOVR R1, k + zero block ; FENCBR ; MADR R0, R0, c.xxxx, R1 + {0.5}
mixed = rows(sys.argv[2])
if len(mixed) != 6:
    raise SystemExit(
        "FAIL: fp_mad_literal_uniform_f must emit six ucode rows (two MOVs "
        "with their blocks, the fence and the MAD with its block), not %d"
        % len(mixed)
    )
if mixed[2] != ZEROS:
    raise SystemExit(
        "FAIL: the preloaded UNIFORM's block must stay zero for the host to "
        "patch; got [%s]" % ", ".join("0x%08x" % w for w in mixed[2])
    )
if mixed[5] != [HALF, 0, 0, 0]:
    raise SystemExit(
        "FAIL: the literal multiplier must reach the MAD's own const block "
        "as 0.5; got [%s].  All zeros there is the defect - the output "
        "becomes the uniform addend alone."
        % ", ".join("0x%08x" % w for w in mixed[5])
    )
if mixed[4][2] != CONST_XXXX:
    raise SystemExit(
        "FAIL: the MAD's multiplier operand must read the const block with "
        "an .xxxx swizzle (0x%08x); got 0x%08x" % (CONST_XXXX, mixed[4][2])
    )
PY

printf 'mad-literal-operand-test: ok\n'
