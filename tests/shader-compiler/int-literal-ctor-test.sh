#!/usr/bin/env bash
# An INT literal in a float constructor converts (t_dc1d92b0).  The IR
# builder's constructor fold accepted float constants only, so one int
# literal stopped the whole constructor folding and the back end saw four
# loose constants where it wanted a literal vec4 - `float4(1,1,1,1)`, the
# most idiomatic constant in the language, did not build.
#
# Compiling is most of the assertion, since the defect was a refusal.  The
# const blocks are asserted too, so a fold that converted the wrong way
# (1 as a bit pattern rather than a value, say) could not pass by merely
# producing a container.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-int-literal-ctor-test.$$"
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
        fail "$2 did not compile.  An int literal in a float constructor
converts in Cg and in the reference compiler; refusing it is the defect
(t_dc1d92b0)."
    fi
}

lit="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_int_literal_ctor_f.cg"
var="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_int_var_ctor_f.cg"
[[ -f "$lit" ]] || fail "fixture missing: $lit"
[[ -f "$var" ]] || fail "fixture missing: $var"

compile "$lit" lit
compile "$var" var

python3 - "$work/lit.log" "$work/var.log" <<'PY'
import re
import sys


def rows(path):
    out = []
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if m:
            out.append([int(w, 16) for w in m.group(2).split()])
    return out


# Const-block words in the byte order the container carries them.
ONE, HALF, QUARTER, EIGHTH = 0x00003F80, 0x00003F00, 0x00003E80, 0x00003E00

lit, var = rows(sys.argv[1]), rows(sys.argv[2])

# float4(1,1,1,1): one distinct value, so one packed lane broadcast.
if len(lit) != 3 or lit[2] != [ONE, 0, 0, 0]:
    raise SystemExit(
        "FAIL: float4(1,1,1,1) must pack a single 1.0 into the const block; "
        "got %d rows, block [%s].  The int literal has to convert to 1.0f, "
        "not to some other reading of the bits."
        % (len(lit), ", ".join("0x%08x" % w for w in (lit[2] if len(lit) > 2 else [])))
    )

# float x = 1; float4(0.5, 0.25, 0.125, x): four distinct values, identity.
if len(var) != 3 or var[2] != [HALF, QUARTER, EIGHTH, ONE]:
    raise SystemExit(
        "FAIL: the int-initialised variable must reach the const block as "
        "1.0 in the w lane; got %d rows, block [%s]"
        % (len(var), ", ".join("0x%08x" % w for w in (var[2] if len(var) > 2 else [])))
    )
PY

printf 'int-literal-ctor-test: ok\n'
