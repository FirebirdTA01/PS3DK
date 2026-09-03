#!/usr/bin/env bash
# t_a7dd471f: asin, acos and atan on the general fragment path.
#
# Oracle shape, measured from sce-cgc before implementation:
#   * atan reuses atan2's minimax polynomial table, with scalar DIVR for a
#     scalar argument and the vector-divide rule for vectors: per-lane RCP
#     plus vector MUL, never vector DIVR.
#   * asin and acos share a separate fitted table.  Their input-side clamp
#     reaches the destination saturate modifier in the oracle, and their
#     sign/quadrant repair uses an SLT value and the m2 destination scale
#     forms (SLTR_m2 for asin, MOVR_m2 for acos), not a CC predicate.
#   * asin/acos vector listings are optimizer-sensitive, but the measured
#     vector oracle uses DIVSQR instead of scalar DIVR for the shared
#     sqrt-multiply shape.
#
# All coefficients are asserted by their emitted bits.  For atan, 0x3f7fffb7
# is fitted and not 1.0, while 0xbeaa7e45 is fitted and not exactly -1/3.
# For asin/acos, 0x3fc90da4 is an observed fitted coefficient near pi/2,
# not the exact pi/2 constant 0x3fc90fdb used later by the repair.
#
# CONTROL: before this slice, all positive fixtures refuse with unsupported
# IROp::Atan/IROp::Asin/IROp::Acos.  The VP fixture must continue to refuse:
# these fragment shapes have not been measured for the vertex path.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-stdlib-trig-poly-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

compile_fp() {
    local stem="$1"
    local src="$shaders/$stem.cg"
    local log="$work/$stem.log"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx --general-lowering "$src"
    ) >"$log" 2>&1 || {
        tail -n 30 "$log" >&2
        fail "$stem did not compile"
    }
}

compile_fp fp_atan_scalar_f
compile_fp fp_atan_lanes_f
compile_fp fp_atan_swizzle_f
compile_fp fp_asin_acos_scalar_f
compile_fp fp_asin_acos_lanes_f
compile_fp fp_asin_acos_swizzle_f

python3 - \
    "$work/fp_atan_scalar_f.log" \
    "$work/fp_atan_lanes_f.log" \
    "$work/fp_atan_swizzle_f.log" \
    "$work/fp_asin_acos_scalar_f.log" \
    "$work/fp_asin_acos_lanes_f.log" \
    "$work/fp_asin_acos_swizzle_f.log" <<'PY'
import re
import sys

MOV, MUL, MAD, MIN, MAX, RCP, RSQ, SGT, SLT, DIV, DIVSQR = (
    0x01, 0x02, 0x04, 0x08, 0x09, 0x1A, 0x1B, 0x0D, 0x0A, 0x3A, 0x3B)
CONST = 2
DST_SCALE_2X = 1
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")

ATAN_COEFFS = {
    "dd30bc5c": "atan minimax c0",
    "6d553d6b": "atan minimax c1",
    "4c31bdf8": "atan minimax c2",
    "54c93e48": "atan minimax c3",
    "7e45beaa": "atan minimax c4, not exactly -1/3",
    "ffb73f7f": "atan minimax c5, not 1.0",
    "0fdb3fc9": "atan pi/2",
}

ASIN_ACOS_COEFFS = {
    "6e30bc99": "asin/acos fitted c0",
    "16273d98": "asin/acos fitted c1",
    "3484be59": "asin/acos fitted c2",
    "0da43fc9": "asin/acos fitted c3, not exact pi/2",
    "0fdb3fc9": "asin/acos pi/2",
    "0fdb4049": "acos pi",
}


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def decode(path):
    groups = []
    for line in open(path, encoding="utf-8"):
        m = LINE.match(line)
        if not m:
            continue
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
            "ccw": bool(w[0] & (1 << 8)),
            "cc_test": bool((w[1] >> 17) & 1),
            "cond": (w[1] >> 18) & 7,
            "src0_type": w[1] & 3,
            "src0_index": (w[1] >> 2) & 0x3F,
            "src1_type": w[2] & 3,
            "src1_index": (w[2] >> 2) & 0x3F,
        })
        i += 1 + (1 if consts else 0)
    return out


def require_bits(path, label, bits):
    raw = open(path, encoding="utf-8").read().lower()
    missing = [name for word, name in bits.items() if word not in raw]
    if missing:
        raise SystemExit("FAIL: %s missing coefficient bits: %s" %
                         (label, ", ".join(missing)))


def require_predicated_repair(ins, label):
    if not any(d["ccw"] for d in ins):
        raise SystemExit("FAIL: %s has no condition-code write" % label)
    if not any(d["cc_test"] for d in ins):
        raise SystemExit("FAIL: %s has no predicated repair" % label)


def require_atan(path, label, lanes):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    require_bits(path, label, ATAN_COEFFS)
    if sum(1 for d in ins if d["op"] == MAD) < 5:
        raise SystemExit("FAIL: %s has fewer than five MADR polynomial stages"
                         % label)
    if not any(d["op"] == SGT and d["ccw"] for d in ins):
        raise SystemExit("FAIL: %s has no SGTRC abs(x)>1 repair" % label)
    require_predicated_repair(ins, label)
    divs = [d for d in ins if d["op"] == DIV]
    rcps = [d for d in ins if d["op"] == RCP]
    if lanes == 1:
        if not divs:
            raise SystemExit("FAIL: %s emitted no scalar DIVR" % label)
        if rcps:
            raise SystemExit("FAIL: %s used RCP/MUL for scalar atan" % label)
    else:
        if divs:
            raise SystemExit(
                "FAIL: %s emitted DIVR for vector atan; the oracle uses "
                "per-lane RCP plus vector MUL" % label)
        if len(rcps) < lanes:
            raise SystemExit("FAIL: %s emitted fewer than %d RCPs" %
                             (label, lanes))
        if any(bin(d["mask"]).count("1") != 1 for d in rcps):
            raise SystemExit("FAIL: %s emitted a multi-lane RCP" % label)
    print("%s: atan polynomial and scalar/vector divide shape present" % label)


def require_asin_acos(path, label, lanes):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    require_bits(path, label, ASIN_ACOS_COEFFS)
    if sum(1 for d in ins if d["op"] == MAD) < 6:
        raise SystemExit("FAIL: %s has too few asin/acos polynomial/repair "
                         "MADR stages" % label)
    if not any(d["sat"] for d in ins if d["op"] in (MOV, MAD)):
        raise SystemExit("FAIL: %s has no destination saturate clamp" % label)
    if not any(d["scale"] == DST_SCALE_2X for d in ins):
        raise SystemExit("FAIL: %s has no m2/2x destination-scale repair" %
                         label)
    if not any(d["op"] == SLT for d in ins):
        raise SystemExit("FAIL: %s has no SLT value for sign repair" % label)
    if lanes == 1:
        if not any(d["op"] == RSQ for d in ins):
            raise SystemExit("FAIL: %s emitted no scalar RSQ" % label)
        divs = [d for d in ins if d["op"] == DIV]
        if not divs:
            raise SystemExit("FAIL: %s emitted no scalar DIVR" % label)
        if any(bin(d["mask"]).count("1") != 1 for d in divs):
            raise SystemExit("FAIL: %s emitted multi-lane scalar DIVR" % label)
    else:
        divsqrt = [d for d in ins if d["op"] == DIVSQR]
        if not divsqrt:
            raise SystemExit(
                "FAIL: %s emitted no DIVSQR for the vector asin/acos "
                "sqrt-multiply shape" % label)
        if len(divsqrt) < lanes:
            raise SystemExit(
                "FAIL: %s emitted fewer than %d per-lane DIVSQRs" %
                (label, lanes))
        if any(bin(d["mask"]).count("1") != 1 for d in divsqrt):
            raise SystemExit(
                "FAIL: %s emitted a multi-lane DIVSQR.  The oracle emits "
                "this sqrt-multiply shape one lane at a time." % label)
        if not any(
            d["src0_type"] == 0 and d["src1_type"] == 0 and
            d["src0_index"] == d["src1_index"]
            for d in divsqrt
        ):
            raise SystemExit(
                "FAIL: %s DIVSQR does not compute sqrt(delta) as "
                "delta/sqrt(delta); using the polynomial as numerator "
                "computes the reciprocal factor and mismatches pixels" %
                label)
        if not any(d["op"] == MUL for d in ins):
            raise SystemExit(
                "FAIL: %s has no MUL applying the polynomial to sqrt(delta)" %
                label)
        scalar_ops = [d for d in ins if d["op"] in (RCP, RSQ, DIV)]
        if any(bin(d["mask"]).count("1") != 1 for d in scalar_ops):
            raise SystemExit(
                "FAIL: %s emitted a multi-lane scalar-unit instruction" %
                label)
    print("%s: asin/acos fitted constants and repair shape present" % label)


require_atan(sys.argv[1], "fp_atan_scalar_f", 1)
require_atan(sys.argv[2], "fp_atan_lanes_f", 3)
require_atan(sys.argv[3], "fp_atan_swizzle_f", 4)
require_asin_acos(sys.argv[4], "fp_asin_acos_scalar_f", 1)
require_asin_acos(sys.argv[5], "fp_asin_acos_lanes_f", 3)
require_asin_acos(sys.argv[6], "fp_asin_acos_swizzle_f", 4)
PY

vp_log="$work/vp_trig_poly_guard_v.log"
vp_rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_vp_rsx --general-lowering "$shaders/vp_trig_poly_guard_v.cg"
) >"$vp_log" 2>&1 || vp_rc=$?
if [[ "$vp_rc" -eq 0 ]]; then
    fail "vp_trig_poly_guard_v compiled; VP asin/acos/atan are unmeasured and must refuse"
fi
grep -Eiq 'unsupported IR op (asin|acos|atan)|VP .*(asin|acos|atan)|unsupported VP VOp' "$vp_log" \
    || fail "vp_trig_poly_guard_v did not refuse with an asin/acos/atan VP diagnostic"

printf 'PASS: stdlib-trig-poly-test\n'
