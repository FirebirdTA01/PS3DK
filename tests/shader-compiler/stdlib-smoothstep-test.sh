#!/usr/bin/env bash
# t_a7dd471f: smoothstep on the general fragment path.
#
# The oracle probe is smoothstep(0.25, 0.75, x).  sce-cgc folds the
# reciprocal of (0.75 - 0.25) into the destination scale and spells the clamp
# as ADDR_2X_sat, then computes t*t*(3 - 2*t).  This test locks that measured
# constant-edge shape: no RCP is needed for the two fixtures here, and the
# first clamp stage must be a saturated ADD with 2X destination scale.
#
# Three fixtures are checked: distinct unswizzled vector lanes, a swizzled
# vector argument, and variable vector edges that cannot fold the reciprocal.
# The swizzled cases are the same trap that caught cbb762e: per-lane lowering
# must compose through arg.swizzle[lane], not raw lane N.  Vector variable-edge
# smoothstep uses the reference's vector-divide shape: one RCP per divisor
# lane and then a saturated MUL, not DIVR.
#
# CONTROL: this fails on compilers before the smoothstep slice because
# IROp::SmoothStep reaches the general lowering as an unsupported op.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-stdlib-smoothstep-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile_fixture() {
    local stem="$1"
    local src="$repo_root/tools/rsx-cg-compiler/tests/shaders/$stem.cg"
    local log="$work/$stem.log"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$log" 2>&1 || {
        tail -n 20 "$log" >&2
        fail "$stem did not compile"
    }
}

compile_fixture fp_smoothstep_lanes_f
compile_fixture fp_smoothstep_swizzle_f
compile_fixture fp_smoothstep_variable_edges_f

python3 - \
    "$work/fp_smoothstep_lanes_f.log" \
    "$work/fp_smoothstep_swizzle_f.log" \
    "$work/fp_smoothstep_variable_edges_f.log" <<'PY'
import re
import sys

ADD, MUL, RCP, DIV = 0x03, 0x02, 0x1A, 0x3A
DST_SCALE_2X = 1
TEMP, CONST = 0, 2
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def decode(path):
    groups = []
    for line in open(path, encoding="utf-8"):
        m = LINE.match(line)
        if m:
            words = [unswap(int(x, 16)) for x in m.group(2).split()]
            if len(words) == 4:
                groups.append(words)
    out, i = [], 0
    while i < len(groups):
        w = groups[i]
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == CONST)
        out.append({
            "op": (w[0] >> 24) & 0x3F,
            "sat": bool(w[0] & (1 << 31)),
            "mask": (w[0] >> 9) & 0xF,
            "scale": (w[2] >> 28) & 0x7,
        })
        i += 1 + (1 if consts else 0)
    return out


def assert_constant_edge(path):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % path)
    if any(d["op"] == RCP for d in ins):
        raise SystemExit(
            "FAIL: %s emitted RCP for smoothstep(0.25, 0.75, x); "
            "the oracle folds this reciprocal into ADDR_2X_sat" % path)
    if not any(d["op"] == ADD and d["sat"] and d["scale"] == DST_SCALE_2X
               for d in ins):
        raise SystemExit(
            "FAIL: %s has no saturated ADD with 2X destination scale for "
            "smoothstep's clamp" % path)
    if sum(1 for d in ins if d["op"] == MUL) < 2:
        raise SystemExit(
            "FAIL: %s does not have the two MUL stages for t*t*(3-2*t)" % path)
    print("%s: smoothstep constant-edge clamp and polynomial shape present"
          % path)


def assert_variable_edge(path):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % path)
    if any(d["op"] == DIV for d in ins):
        raise SystemExit(
            "FAIL: %s emitted DIVR for vector smoothstep.  The oracle uses "
            "per-lane RCP plus vector MUL for vector divides." % path)
    rcps = [d for d in ins if d["op"] == RCP]
    if len(rcps) < 3:
        raise SystemExit(
            "FAIL: %s emitted fewer than three RCPs for a vec3 variable edge "
            "span" % path)
    if any(bin(d["mask"]).count("1") > 1 for d in rcps):
        raise SystemExit(
            "FAIL: %s emitted a multi-lane RCP for a vector edge span; RCP is "
            "a scalar-unit op and must be per-lane" % path)
    if not any(d["op"] == MUL and d["sat"] for d in ins):
        raise SystemExit(
            "FAIL: %s has no saturated MUL for the generic smoothstep clamp"
            % path)
    if sum(1 for d in ins if d["op"] == MUL) < 3:
        raise SystemExit(
            "FAIL: %s does not have RCP clamp plus t*t*(3-2*t) MUL stages"
            % path)
    print("%s: smoothstep variable-edge RCP clamp and polynomial shape present"
          % path)


assert_constant_edge(sys.argv[1])
assert_constant_edge(sys.argv[2])
assert_variable_edge(sys.argv[3])
PY

printf 'PASS: stdlib-smoothstep-test\n'
