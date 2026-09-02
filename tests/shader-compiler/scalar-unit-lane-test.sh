#!/usr/bin/env bash
# On a shader whose lanes must DIFFER, a scalar-unit instruction may write
# only one lane (t_249b8088).
#
# RCP, RSQ, EX2, LG2, COS and SIN are computed by NV40's scalar unit: each
# reads a single source COMPONENT and writes that one result into every
# enabled destination lane.  So `sin(v)` on a float4, emitted as a single
# SINR with a four-lane mask, computes sin(v.x) and stores it in y, z and w
# as well.  It compiles, it declares the right register count, it survives
# the dead-store and collision censuses because the register IS written -
# and three lanes are silently wrong.  It painted 4096 of 4096 pixels wrong
# on the rig while every corpus shader stayed green, because every corpus
# use of these ops is on a float.  The reference emits one instruction per
# lane, each naming its own component: `SINR R0.y, R0.y` and so on.
#
# WHY THIS IS SCOPED TO ONE FIXTURE AND NOT SWEPT OVER THE CORPUS.  A
# multi-lane scalar-unit write is CORRECT when the lanes are meant to hold
# the same value - a broadcast.  The reference does exactly that: on
# fp_pow_computed_literal_f, whose `color = float4(lit, lit, lit, 1)` needs
# one value in three lanes, sce-cgc emits `EX2R R0.xyz, R0` and our output
# is byte-identical to it.  Asserting "one lane always" over the corpus
# therefore fails on correct, oracle-matching output - measured, on the
# first version of this file, which is why the rule is stated over a shader
# whose lanes CANNOT legitimately agree.
#
# The witness builds four distinct lanes from one varying, puts them
# through sin, rsqrt and pow, and reads lanes y, z and w of the results, so
# no broadcast can be right anywhere in it.
#
# CONTROL: this fails on any compiler before the per-lane fix -
#     bash tests/shader-compiler/scalar-unit-lane-test.sh <old-binary>
# names the full-mask SINR and LG2R.  CI runs it against the compiler it
# just built, which proves the invariant holds there; it cannot build an
# old binary to prove the check still bites, so the recipe above is the
# local control.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-scalar-unit-lane-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

witness="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_scalar_unit_lanes_f.cg"
[[ -f "$witness" ]] || fail "witness fixture missing: $witness
it is the shader that puts four distinct lanes through sin, rsqrt and pow"

(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
        -p sce_fp_rsx "$witness"
) >"$work/witness.log" 2>&1 || {
    tail -n 20 "$work/witness.log" >&2
    fail "the witness did not compile"
}

python3 - "$work/witness.log" <<'PY'
import re
import sys

# nvfx_shader.h: the scalar unit's opcodes.
SCALAR = {0x1A: "RCP", 0x1B: "RSQ", 0x1C: "EX2",
          0x1D: "LG2", 0x22: "COS", 0x23: "SIN"}
LINE = re.compile(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


words = []
for line in open(sys.argv[1], encoding="utf-8"):
    m = LINE.match(line)
    if m:
        w = [unswap(int(x, 16)) for x in m.group(2).split()]
        if len(w) == 4:
            words.append(w)
if not words:
    raise SystemExit("FAIL: the witness ucode dump did not parse, so nothing "
                     "below was checked")

instrs, i = [], 0
while i < len(words):
    w = words[i]
    # a CONST source names an inline 16-byte constant block: data, not an
    # instruction, and it must not be decoded as an opcode.
    consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == 2)
    instrs.append(w)
    i += 1 + (1 if consts else 0)

seen, bad = 0, []
for n, w in enumerate(instrs):
    if w[0] & (1 << 30):              # no destination register
        continue
    opcode = (w[0] >> 24) & 0x3F
    if opcode not in SCALAR:
        continue
    seen += 1
    mask = (w[0] >> 9) & 0xF
    if sum(1 for b in range(4) if mask & (1 << b)) > 1:
        bad.append((n, SCALAR[opcode], mask))

if seen == 0:
    raise SystemExit("FAIL: the witness emitted no scalar-unit instruction at "
                     "all - its premise is gone, not its assertion")

for n, op, mask in bad:
    sys.stderr.write(
        "FAIL: instruction %d is %sR with write mask 0x%X.  The scalar unit "
        "computes ONE lane, and this shader's lanes are distinct by "
        "construction, so this stores f(one component) where several "
        "different values belong (t_249b8088).\n" % (n, op, mask))
if bad:
    raise SystemExit(1)

print("scalar-unit-lane: %d scalar-unit instructions in the witness, each "
      "writing a single lane" % seen)
PY

printf 'PASS: scalar-unit-lane-test\n'
