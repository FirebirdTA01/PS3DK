#!/usr/bin/env bash
# A HALF-TYPED VALUE IS COMPUTED IN HALF.
#
# The director's ruling (2026-09-07): "Half type rendering at fp32 is wrong.
# The whole reason to use half types is performance and if we don't honor the
# correct fp16 when declaring them then that is incorrect."  We honoured the
# half OUTPUT - the colour reached H0 - and computed everything feeding it in
# fp32, converting once at the store.  Gemini measured the cost on pixels:
# dullMetalFp differs from the reference on 213 pixels by up to 3/255 because
# the reference keeps a whole normalize at prec=1 and we did not.
#
# THE RULE, measured on sce-cgc 475 (C:/cgdev/h0/hp, 2026-09-07):
#
#   float4 a = p * 1.003; return (half4)a;         MUL R0 prec=0, then H0
#   half4  a = p * (half)1.003; return a;          MOV H0 prec=1, MUL H0 prec=1
#   half4  a = p * (half)1.003; return (float4)a;  MOV H0 prec=1, MUL R0 prec=1
#
# The third row is why this test asserts PRECISION and BANK separately: the
# destination there is a float register and the multiply is still prec=1, so
# the precision follows the VALUE's type and the bank does not.  The first row
# is the control that keeps the two apart in the other direction - float
# values under a half output stay prec=0 (codex's 15-shape matrix) - and it
# is red on any change that reads precision off the output declaration.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-half-arithmetic.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
decoder="$repo_root/tests/shader-compiler/fp_sources.py"

# stdout and stderr are captured SEPARATELY here as everywhere else that
# reads a compiler dump; the diagnostics are read from the stderr half.
emit() {   # <stem>
    local stem="$1" rc=0
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || rc=$?
    [[ "$rc" -ne 124 ]] || fail "$stem timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$stem.err" >&2
        fail "$stem did not compile"
    fi
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
    python3 "$decoder" "$work/$stem.fpo" >"$work/$stem.srcs"
    [[ -s "$work/$stem.srcs" ]] || fail "$stem decoded to no instructions"
}

emit fp_half_arith_chain_f
emit fp_half_value_float_out_f
emit fp_float_value_half_out_f
emit fp_half_ctor_float_lanes_f
emit fp_half_insert_chain_f
emit fp_half_computed_insert_f

python3 - "$work" <<'PY'
import io
import re
import sys

work = sys.argv[1]
ROW = re.compile(r"^(\d+) (\S+) dst=(\S+) mask=(\S+) prec=(\d)")

def rows(stem):
    out = []
    for line in io.open("%s/%s.srcs" % (work, stem), encoding="utf-8"):
        m = ROW.match(line)
        if m:
            out.append((int(m.group(1)), m.group(2), m.group(3), int(m.group(5))))
    if not out:
        raise SystemExit("FAIL: %s decoded to no rows" % stem)
    return out

# An instruction that COMPUTES rather than moves.  A MOV into an H register
# is a half move whatever the value's type, so the moves cannot tell the two
# rules apart - the arithmetic can.
ARITH = {"MUL", "MAD", "ADD", "DP3", "DP4", "RCP", "RSQ", "MIN", "MAX"}

def arith(stem):
    return [r for r in rows(stem) if r[1] in ARITH]

# 1. A CHAIN OF HALF TEMPS IS COMPUTED IN HALF, every instruction of it.
chain = arith("fp_half_arith_chain_f")
if not chain:
    raise SystemExit("FAIL: the half chain emitted no arithmetic at all - the "
                     "fixture folded away and this test would pass vacuously")
bad = [r for r in chain if r[3] != 1]
if bad:
    raise SystemExit(
        "FAIL: half arithmetic computed at fp32.  %s is prec=%d, expected 1.  "
        "A half temp exists so the hardware computes it in fp16; converting "
        "only at the store gives the fp32 result rounded once, which is not "
        "the same picture - the reference keeps the whole chain at prec=1."
        % (bad[0][1], bad[0][3]))

# 2. A HALF VALUE UNDER A FLOAT OUTPUT is still half arithmetic.  This is the
#    row that proves the precision is not read off the destination bank.
value = arith("fp_half_value_float_out_f")
if not value:
    raise SystemExit("FAIL: the half-value/float-output fixture emitted no "
                     "arithmetic")
bad = [r for r in value if r[3] != 1]
if bad:
    raise SystemExit(
        "FAIL: a half value under a FLOAT output computed at prec=%d.  The "
        "reference emits MUL R0 at prec=1 here: the bank follows the output, "
        "the precision follows the value." % bad[0][3])

# 3. THE CONTROL, and it is the one that fails if anyone decides a half
#    OUTPUT means half arithmetic: float values under a half output stay
#    fp32, with only the move into the colour half.
control = arith("fp_float_value_half_out_f")
if not control:
    raise SystemExit("FAIL: the float-value/half-output control emitted no "
                     "arithmetic, so it controls nothing")
bad = [r for r in control if r[3] != 0]
if bad:
    raise SystemExit(
        "FAIL: FLOAT arithmetic under a half output computed at prec=%d, "
        "expected 0.  The reference computes `float4 a = p * 1.003` in full "
        "precision and only the write into the colour is half (codex's "
        "15-shape matrix).  Reading precision off the OUTPUT declaration is "
        "what this row exists to catch." % bad[0][3])

# 4. A HALF CONSTRUCTOR OVER A FLOAT-TYPED PRODUCER must not drag the
#    producer's register into the half bank.  cross() is float3 in the IR
#    even on half inputs, so `half4(cross(p.xyz, p.zyx), p.w)` writes xyz as
#    FLOAT data through the constructor's shared base and only the w lane is
#    half-typed; stamping the register fp16 for that one lane reinterprets
#    xyz and the pixel is wrong (codex).  Every computing instruction here
#    stays in a float register at prec=0, and the ONLY half destination is
#    the move into the colour.
lanes = rows("fp_half_ctor_float_lanes_f")
computing = [r for r in lanes if r[1] in ARITH]
if not computing:
    raise SystemExit("FAIL: the half-constructor fixture emitted no "
                     "arithmetic, so it constrains nothing")
bad = [r for r in computing if r[2].startswith("H") or r[3] != 0]
if bad:
    raise SystemExit(
        "FAIL: %s writes %s at prec=%d in a half constructor over a FLOAT "
        "producer.  cross() is float-typed in the IR, so those lanes hold "
        "float data; putting the register in the half bank reinterprets "
        "them." % (bad[0][1], bad[0][2], bad[0][3]))
half_dsts = [r for r in lanes if r[2].startswith("H")]
if len(half_dsts) != 1 or half_dsts[0][1] != "MOV":
    raise SystemExit(
        "FAIL: the half constructor should reach the half bank exactly once, "
        "in the move into the colour; it wrote H in %d instructions (%s)"
        % (len(half_dsts), ", ".join("%s->%s" % (r[1], r[2]) for r in half_dsts)))

# 5. A HALF VALUE WHOSE REGISTER IS WRITTEN AGAIN keeps ONE bank.  Lane
#    inserts append writes to a register earlier instructions already own;
#    if those appended writes use the R view while the earlier ones used H,
#    the register holds packed half data that the conversion at the end
#    reads as float.  Both directions are wrong and only one of them is
#    caught by the constructor row above, so the two shapes are pinned here:
#    a copy-based chain and a computed one.
for stem in ("fp_half_insert_chain_f", "fp_half_computed_insert_f"):
    banks = set()
    for r in rows(stem):
        if r[2] in ("none",):
            continue
        banks.add(r[2][0])
    if banks != {"H"}:
        raise SystemExit(
            "FAIL: %s writes more than one register bank (%s).  Every write "
            "to a half value's register has to agree with the writes before "
            "it; mixing the H and R views of the same register means the "
            "conversion at the end reads half data as float."
            % (stem, ", ".join(sorted(banks))))
    slow = [r for r in rows(stem) if r[3] != 1]
    if slow:
        raise SystemExit(
            "FAIL: %s emits %s at prec=%d - a half chain computes in half "
            "throughout" % (stem, slow[0][1], slow[0][3]))

print("half-arithmetic: chain %d arithmetic instructions at prec=1, half "
      "value under a float output prec=1, float value under a half output "
      "prec=0, float producer under a half constructor stays float in %d "
      "instructions, both half insert chains single-bank at prec=1"
      % (len(chain), len(computing)))
PY

printf 'half-arithmetic: PASS\n'
