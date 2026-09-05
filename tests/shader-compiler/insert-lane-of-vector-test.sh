#!/usr/bin/env bash
# A per-lane write from a computed vector reads ITS OWN lane (t_856689b2).
#
# The general path's VecInsert lowering forced the scalar source's swizzle
# to lane 0.  Right for a literal - lane 0 of the const block - and wrong
# for a lane extract, whose lane resolve() had already selected.  So
# `color.y = lit.y` emitted `MOV R0.y, R16.x`, the red channel broadcast
# into green and blue, on four shaders whose reference paints three
# distinct channels.
#
# THIS ASSERTION IS ABOUT THIS FIXTURE, not a general rule.  `c.y = v.x` is
# a perfectly good program and would fail the check below; what makes the
# check sound here is the fixture's source, which writes lit.x, lit.y and
# lit.z into x, y and z.  A shader is only evidence for the rule its own
# text states.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_insert_lanes_of_vector_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

work="${TMPDIR:-/tmp}/ps3dk-insert-lane-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$work/general.log" 2>&1 || rc=$?
[[ "$rc" -eq 124 ]] && fail "fp_insert_lanes_of_vector timed out"
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/general.log" >&2
    fail "fp_insert_lanes_of_vector did not compile on the general path"
fi

python3 - "$work/general.log" <<'PY'
import re
import sys


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


REG_TYPE_TEMP = 0
MOV = 0x01

# destination lane mask -> the lane the source must read
want = {0x1: 0, 0x2: 1, 0x4: 2}
seen = {}

for line in open(sys.argv[1], "r", encoding="utf-8"):
    m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
    if not m:
        continue
    w = [unswap(int(x, 16)) for x in m.group(2).split()]
    if len(w) < 4:
        continue
    if ((w[0] >> 24) & 0x3F) != MOV:
        continue
    mask = (w[0] >> 9) & 0xF
    if mask not in want:
        continue
    if (w[1] & 3) != REG_TYPE_TEMP:
        continue          # the literal w lane comes from a const block
    lane = (w[1] >> 9) & 3
    seen[mask] = lane

missing = [m for m in want if m not in seen]
if missing:
    raise SystemExit(
        "FAIL: expected three single-lane MOVs from a temp writing x, y and "
        "z; masks found: %s.  Nothing was checked."
        % ", ".join("0x%x" % k for k in sorted(seen))
    )

wrong = {m: (seen[m], want[m]) for m in want if seen[m] != want[m]}
if wrong:
    detail = "; ".join(
        "lane %s reads %s, should read %s"
        % ("xyzw"[want[m]], "xyzw"[got], "xyzw"[exp])
        for m, (got, exp) in sorted(wrong.items())
    )
    raise SystemExit(
        "FAIL: a per-lane write reads the wrong lane of its source - " +
        detail + ".  Forcing the source swizzle to lane 0 broadcasts the "
        "first channel into the others (t_856689b2)."
    )
PY

printf 'insert-lane-of-vector-test: ok\n'
