#!/usr/bin/env bash
# t_a7dd471f: atan2 on the general fragment path.
#
# Oracle shape, measured from sce-cgc before implementation:
#   scalar atan2 uses MIN/MAX of the absolute inputs, scalar DIVR, SGTRC,
#   a five-MADR Horner polynomial in t*t, MUL by t, CC-predicated quadrant
#   repair, and FENCBR before the final move.
#
# Vector atan2 is deliberately different: the oracle does NOT use DIVR for
# vector division.  It emits per-lane RCP on MAX(abs(y), abs(x)), one vector
# MUL with MIN(abs(y), abs(x)), then the vector polynomial and per-lane
# predicated repair.  The swizzle fixture catches the raw-lane bug that
# cbb762e had in exp/log; every per-lane source must compose through the
# argument's swizzle.
#
# The polynomial constants are a minimax fit, not a Taylor series.  In
# particular 0x3f7fffb7 is 0.9999956489, not 1.0, and 0xbeaa7e45 is not
# exactly -1/3.  All constants are asserted by their emitted bits so later
# "cleanup" cannot silently change the oracle coefficients.
#
# CONTROL: before this slice, all positive fixtures refuse with unsupported
# IROp::Atan2.  The VP fixture must continue to refuse: the vertex atan2
# lowering is unmeasured, and any new VOp reaching VP without a case now
# refuses rather than falling through to MOV.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-stdlib-atan2-test.$$"
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
            -p sce_fp_rsx "$src"
    ) >"$log" 2>&1 || {
        tail -n 30 "$log" >&2
        fail "$stem did not compile"
    }
}

compile_fp fp_atan2_scalar_f
compile_fp fp_atan2_lanes_f
compile_fp fp_atan2_swizzle_f

python3 - \
    "$work/fp_atan2_scalar_f.log" \
    "$work/fp_atan2_lanes_f.log" \
    "$work/fp_atan2_swizzle_f.log" <<'PY'
import re
import sys

MOV, MUL, MAD, MIN, MAX, RCP, SGT, DIV = 0x01, 0x02, 0x04, 0x08, 0x09, 0x1A, 0x0D, 0x3A
CONST = 2
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")
COEFFS = {
    "dd30bc5c": "atan2 minimax c0",
    "6d553d6b": "atan2 minimax c1",
    "4c31bdf8": "atan2 minimax c2",
    "54c93e48": "atan2 minimax c3",
    "7e45beaa": "atan2 minimax c4",
    "ffb73f7f": "atan2 minimax c5, not 1.0",
    "0fdb3fc9": "atan2 pi/2",
    "0fdb4049": "atan2 pi",
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
            "ccw": bool(w[0] & (1 << 8)),
            "cc_test": bool((w[1] >> 17) & 1),
            "cond": (w[1] >> 18) & 7,
        })
        i += 1 + (1 if consts else 0)
    return out


def require_coeffs(path, label):
    raw = open(path, encoding="utf-8").read().lower()
    missing = [name for word, name in COEFFS.items() if word not in raw]
    if missing:
        raise SystemExit("FAIL: %s missing coefficient bits: %s" %
                         (label, ", ".join(missing)))


def require_common(path, label):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    if any(d["sat"] for d in ins if d["op"] < 0x3E):
        raise SystemExit(
            "FAIL: %s used a destination saturate modifier; the atan2 oracle "
            "shape has none" % label)
    if sum(1 for d in ins if d["op"] == MAD) < 5:
        raise SystemExit("FAIL: %s has fewer than five MADR polynomial stages" %
                         label)
    if not any(d["op"] == SGT and d["ccw"] for d in ins):
        raise SystemExit("FAIL: %s has no SGTRC condition-code write" % label)
    if not any(d["cc_test"] for d in ins):
        raise SystemExit("FAIL: %s has no predicated quadrant/sign repair" %
                         label)
    if not any(d["cc_test"] and d["cond"] == 5 for d in ins):
        raise SystemExit("FAIL: %s has no NE-predicated quadrant repair" %
                         label)
    try:
        quad = next(i for i, d in enumerate(ins)
                    if d["cc_test"] and d["cond"] == 5)
    except StopIteration:
        raise SystemExit("FAIL: %s has no NE-predicated quadrant repair" %
                         label)
    lt_repairs = [i for i, d in enumerate(ins)
                  if d["cc_test"] and d["cond"] == 1 and i > quad]
    if len(lt_repairs) < 2:
        raise SystemExit("FAIL: %s has fewer than two LT-predicated sign "
                         "repairs" % label)
    x_repair, y_repair = lt_repairs[0], lt_repairs[-1]
    if not any(d["ccw"] for d in ins[quad + 1:x_repair]):
        raise SystemExit("FAIL: %s reuses the quadrant CC for the x-sign "
                         "repair; the scheduler moved the x MOVRC above the "
                         "SGTRC" % label)
    if not any(d["ccw"] for d in ins[x_repair + 1:y_repair]):
        raise SystemExit("FAIL: %s reuses the x-sign CC for the y-sign "
                         "repair; the scheduler moved the y MOVRC above its "
                         "consumer" % label)
    require_coeffs(path, label)
    return ins


def require_scalar(path, label):
    ins = require_common(path, label)
    divs = [d for d in ins if d["op"] == DIV]
    if not divs:
        raise SystemExit("FAIL: %s emitted no scalar DIVR" % label)
    if any(d["mask"] not in (0x1, 0x2, 0x4, 0x8) for d in divs):
        raise SystemExit("FAIL: %s emitted multi-lane DIVR" % label)
    if any(d["op"] == RCP for d in ins):
        raise SystemExit("FAIL: %s used RCP/MUL for scalar atan2 division" %
                         label)
    print("%s: scalar atan2 uses DIVR and predicated polynomial repair" % label)


def require_vector(path, label, lanes):
    ins = require_common(path, label)
    if any(d["op"] == DIV for d in ins):
        raise SystemExit(
            "FAIL: %s emitted DIVR for vector atan2; the oracle uses RCP/MUL" %
            label)
    rcps = [d for d in ins if d["op"] == RCP]
    if len(rcps) < lanes:
        raise SystemExit("FAIL: %s emitted fewer than %d RCPs" % (label, lanes))
    if any(bin(d["mask"]).count("1") != 1 for d in rcps):
        raise SystemExit("FAIL: %s emitted a multi-lane RCP" % label)
    if not any(d["op"] == MIN and d["mask"] == ((1 << lanes) - 1)
               for d in ins):
        raise SystemExit("FAIL: %s has no vector MIN(abs(y), abs(x)) stage" %
                         label)
    print("%s: vector atan2 keeps per-lane RCP plus vector MUL" % label)


require_scalar(sys.argv[1], "fp_atan2_scalar_f")
require_vector(sys.argv[2], "fp_atan2_lanes_f", 3)
require_vector(sys.argv[3], "fp_atan2_swizzle_f", 4)
PY

vp_log="$work/vp_atan2_guard_v.log"
vp_rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_vp_rsx "$shaders/vp_atan2_guard_v.cg"
) >"$vp_log" 2>&1 || vp_rc=$?
if [[ "$vp_rc" -eq 0 ]]; then
    fail "vp_atan2_guard_v compiled; VP atan2 is unmeasured and must refuse"
fi
grep -Eiq 'unsupported IR op atan2|unsupported op atan2|VP .*atan2|unsupported VP VOp' "$vp_log" \
    || fail "vp_atan2_guard_v did not refuse with an atan2/VP diagnostic"

printf 'PASS: stdlib-atan2-test\n'
