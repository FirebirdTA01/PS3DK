#!/usr/bin/env bash
# An INT literal in a float constructor converts (integer-literal-constructor-conversion).  The IR
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
        fail "$2 did not compile.  An int literal in a float constructor
converts in Cg and in the reference compiler; refusing it is the defect
(integer-literal-constructor-conversion)."
    fi
}

lit="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_int_literal_ctor_f.cg"
var="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_int_var_ctor_f.cg"
[[ -f "$lit" ]] || fail "fixture missing: $lit"
[[ -f "$var" ]] || fail "fixture missing: $var"

compile "$lit" lit_general
compile "$var" var_general

python3 - "$work/lit_general.log" "$work/var_general.log" <<'PY'
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

# General lowering may materialise a full vec4 constant instead
# of a packed scalar broadcast.  The visible property is that integer
# literals convert to floating values before they reach the emitted
# container; refusing was the original defect, and bit-pattern conversion
# would still compile but paint the wrong value.
lit_general = rows(sys.argv[1])
if [ONE, ONE, ONE, ONE] not in lit_general and [ONE, 0, 0, 0] not in lit_general:
    raise SystemExit(
        "FAIL: general float4(1,1,1,1) must contain 1.0f after int literal "
        "conversion; const/data rows were [%s] (integer-literal-constructor-conversion)."
        % "; ".join(",".join("0x%08x" % w for w in r)
                     for r in lit_general)
    )

var_general = rows(sys.argv[2])
if [HALF, QUARTER, EIGHTH, ONE] not in var_general:
    raise SystemExit(
        "FAIL: general int-initialised variable must reach the w lane as "
        "1.0f; const/data rows were [%s] (integer-literal-constructor-conversion)."
        % "; ".join(",".join("0x%08x" % w for w in r)
                     for r in var_general)
    )
PY

printf 'int-literal-ctor-test: ok\n'
