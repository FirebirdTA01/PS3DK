#!/usr/bin/env bash
# A 4D dot lowers to DP4 and a 3D dot to DP3 (t_856689b2's last one).
#
# The general path chose between them on the RESULT's width, and a dot
# product's result is a scalar - always - so the test was never true and
# every dot became DP3.  A four-component dot silently dropped its w term.
#
# The fixture contains BOTH widths, and the test requires one of each.  A
# fix that always chose DP4 would pass a DP4-only fixture and break every
# lighting shader in the corpus; that is the failure this shape rules out.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_dot4_width_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

work="${TMPDIR:-/tmp}/ps3dk-dot-width-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$work/general.log" 2>&1 || rc=$?
[[ "$rc" -eq 124 ]] && fail "fp_dot4_width timed out"
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/general.log" >&2
    fail "fp_dot4_width did not compile on the general path"
fi

python3 - "$work/general.log" <<'PY'
import re
import sys

DP3, DP4 = 0x05, 0x06


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


counts = {DP3: 0, DP4: 0}
for line in open(sys.argv[1], "r", encoding="utf-8"):
    m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
    if not m:
        continue
    w = [unswap(int(x, 16)) for x in m.group(2).split()]
    if len(w) < 4:
        continue
    op = (w[0] >> 24) & 0x3F
    if op in counts:
        counts[op] += 1

if counts[DP4] != 1 or counts[DP3] != 1:
    raise SystemExit(
        "FAIL: the fixture has one 4D dot and one 3D dot, so the ucode must "
        "carry exactly one DP4 and one DP3; it has %d DP4 and %d DP3.  Two "
        "DP3s means the 4D dot dropped its w term - the width that chooses "
        "is the OPERANDS', not the scalar result's (t_856689b2)."
        % (counts[DP4], counts[DP3])
    )
PY

printf 'dot-width-test: ok (one DP4, one DP3)\n'
