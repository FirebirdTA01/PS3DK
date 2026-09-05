#!/usr/bin/env bash
# t_58e2212c: NV40 fragment DIVR (opcode 0x3A) is the oracle's divide
# shape for tan, smoothstep's dynamic edge clamp, and the trig polynomial
# family.  RCP-then-MUL is correct enough for some pixels but is not the
# reference shape and blocks the rest of the stdlib lane.
#
# This test deliberately has two halves:
#   * fragment fixtures require DIVR, including a saturated DIVR for
#     scalar variable-edge smoothstep;
#   * a VP fixture still refuses by name.  The vertex unit's DIV encoding is
#     unmeasured, so DIVR must not reach VP emission until that path has its
#     own oracle fixture.
#
# CONTROL: before the DIVR lowering, the positive fragment fixtures compile
# through RCP/MUL and this test names the missing 0x3A opcode.  The vector
# smoothstep fixture is the known-good control: the reference does NOT use
# DIVR for vector division, so it must keep the RCP/MUL idiom.  The literal
# divisor fixtures are the other negative controls: the reference folds
# /3.0 to MUL by the correctly rounded reciprocal 0x3eaaaaab, and folds
# vector /0.01 to vector MUL by 100, so neither should emit DIVR or RCP.
# The scaled-length fixture names the shape test_83 exposed after the literal
# fold made it compile: sce-cgc computes length(x) * k as RSQ followed by
# DIVR k, rsq, not DIVSQR followed by MUL.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-divr-opcode-test.$$"
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
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$log" 2>&1 || {
        tail -n 20 "$log" >&2
        fail "$stem did not compile"
    }
}

compile_fp fp_divr_scalar_f
compile_fp fp_tan_divr_f
compile_fp fp_smoothstep_scalar_variable_edges_f
compile_fp fp_smoothstep_variable_edges_f
compile_fp fp_div_literal_recip_f
compile_fp fp_div_literal_vector_recip_f
compile_fp fp_length_scaled_divr_f

python3 - \
    "$work/fp_divr_scalar_f.log" \
    "$work/fp_tan_divr_f.log" \
    "$work/fp_smoothstep_scalar_variable_edges_f.log" \
    "$work/fp_smoothstep_variable_edges_f.log" \
    "$work/fp_div_literal_recip_f.log" \
    "$work/fp_div_literal_vector_recip_f.log" \
    "$work/fp_length_scaled_divr_f.log" <<'PY'
import re
import sys

DIV, RCP, MUL, RSQ, DIVSQR = 0x3A, 0x1A, 0x02, 0x1B, 0x3B
CONST = 2
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")


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
        })
        i += 1 + (1 if consts else 0)
    return out


def assert_divr(path, label, saturated):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    divs = [d for d in ins if d["op"] == DIV]
    if not divs:
        raise SystemExit(
            "FAIL: %s emitted no DIVR (0x3A); the old RCP/MUL idiom is still "
            "in use" % label)
    if any(d["mask"] not in (0x1, 0x2, 0x4, 0x8) for d in divs):
        raise SystemExit(
            "FAIL: %s emitted a multi-lane DIVR.  The oracle uses DIVR for "
            "scalar division only; vector divides stay RCP/MUL." % label)
    if saturated and not any(d["sat"] for d in divs):
        raise SystemExit(
            "FAIL: %s emitted DIVR but not DIVR_sat for the clamp" % label)
    if not saturated and any(d["sat"] for d in divs):
        raise SystemExit("FAIL: %s unexpectedly saturated its DIVR" % label)
    if any(d["op"] == RCP for d in ins):
        raise SystemExit(
            "FAIL: %s still emitted RCP (0x1A) on a DIVR-covered divide path"
            % label)
    print("%s: %d DIVR instruction(s), saturated=%s" %
          (label, len(divs), saturated))


def assert_vector_rcp_mul(path, label):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    if any(d["op"] == DIV for d in ins):
        raise SystemExit(
            "FAIL: %s emitted DIVR for vector division.  The oracle uses "
            "per-lane RCP plus vector MUL for vector divides." % label)
    rcps = [d for d in ins if d["op"] == RCP]
    if len(rcps) < 3:
        raise SystemExit(
            "FAIL: %s emitted fewer than three RCPs for a vec3 variable edge "
            "span" % label)
    if any(bin(d["mask"]).count("1") > 1 for d in rcps):
        raise SystemExit(
            "FAIL: %s emitted a multi-lane RCP for a vector divide; RCP is a "
            "scalar-unit op and must be per-lane" % label)
    print("%s: vector divide stayed on per-lane RCP/MUL" % label)


def assert_literal_recip_mul(path, label, expected_word):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    if any(d["op"] == DIV for d in ins):
        raise SystemExit(
            "FAIL: %s emitted DIVR for division by a literal divisor.  The "
            "oracle folds this to MUL by the reciprocal." % label)
    if any(d["op"] == RCP for d in ins):
        raise SystemExit(
            "FAIL: %s still emitted RCP for division by a literal divisor; "
            "the reciprocal should be folded before emission." % label)
    if not any(d["op"] == MUL for d in ins):
        raise SystemExit("FAIL: %s emitted no MUL for the reciprocal" % label)
    raw = open(path, encoding="utf-8").read().lower()
    if expected_word not in raw:
        raise SystemExit(
            "FAIL: %s did not emit the oracle's reciprocal bits %s" %
            (label, expected_word))
    print("%s: literal divisor folded to MUL, no DIVR/RCP" % label)


def assert_scaled_length_divr(path, label):
    ins = decode(path)
    if not ins:
        raise SystemExit("FAIL: %s produced no decoded instructions" % label)
    if not any(d["op"] == RSQ for d in ins):
        raise SystemExit(
            "FAIL: %s emitted no RSQ for the oracle's scaled-length shape" %
            label)
    if not any(d["op"] == DIV for d in ins):
        raise SystemExit(
            "FAIL: %s emitted no DIVR for length(x) * literal; test_83 stays "
            "on the one-ULP DIVSQR/MUL path" % label)
    if any(d["op"] == DIVSQR for d in ins):
        raise SystemExit(
            "FAIL: %s still emitted DIVSQR for a scaled length; the oracle "
            "uses RSQ plus DIVR" % label)
    print("%s: scaled length uses RSQ plus DIVR" % label)


assert_divr(sys.argv[1], "fp_divr_scalar_f", False)
assert_divr(sys.argv[2], "fp_tan_divr_f", False)
assert_divr(sys.argv[3], "fp_smoothstep_scalar_variable_edges_f", True)
assert_vector_rcp_mul(sys.argv[4], "fp_smoothstep_variable_edges_f")
assert_literal_recip_mul(sys.argv[5], "fp_div_literal_recip_f", "aaab3eaa")
assert_literal_recip_mul(sys.argv[6], "fp_div_literal_vector_recip_f", "000042c8")
assert_scaled_length_divr(sys.argv[7], "fp_length_scaled_divr_f")
PY

vp_log="$work/vp_divr_guard_v.log"
vp_rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_vp_rsx "$shaders/vp_divr_guard_v.cg"
) >"$vp_log" 2>&1 || vp_rc=$?
if [[ "$vp_rc" -eq 0 ]]; then
    fail "vp_divr_guard_v compiled; DIVR must not reach the unmeasured VP path"
fi
grep -Eq 'VP div lowering deferred|unsupported VP VOp' "$vp_log" \
    || fail "vp_divr_guard_v did not refuse with a VP DIVR/div diagnostic"

printf 'PASS: divr-opcode-test\n'
