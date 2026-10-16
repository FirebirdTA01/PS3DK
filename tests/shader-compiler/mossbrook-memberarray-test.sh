#!/usr/bin/env bash
# MOSSBROOK MEMBER-ARRAY REGRESSION (t_c773d446).
#
# Two refusals used to stop six PBR/terrain fragment shaders (standard_f,
# standardHDR_f, standardEnvmap_f, terrain_f, terrainSimple_f, terrainHDR_f):
#   (a) "scalar-vector broadcast requires numeric element types"
#   (b) "array-bearing struct copy requires a tracked source (member-array-storage)"
#
 # BOTH are now lifted by targeted fixes in ir_builder.cpp (see commit):
 #   (a)  buildCallExpr non-inlined path binds each entry-reachable out/inout
 #        parameter to a typed IROp::Undef in the standalone helper body so a
 #        `mirror[i] = s * 2.0` broadcast no longer sees an un-typed source.
 #   (b)  inlineUserFunctionCall scope-restore remap carries the callee's
 #        tracked array/leaf fields back under the caller's destination
 #        (`structReturnDest_`, keyed by the EXACT CallExpr being initialised)
 #        so a nested helper called from a body, or a call acting as an
 #        argument, cannot claim the caller's destination (see the lead's
 #        `wrap(compute(...))` case).  buildDeclStmt then skips the
 #        caller-side copy for exactly that call (`structReturnInstalled_`).
  #        The remap is installed only when the callee returns EXACTLY ONE
  #        struct local (`inlineStructReturnBaseSet_.size() == 1`) AND builds
  #        ZERO non-local returns (no call / parameter / constructor / member
  #        / ternary arm: `inlineStructReturnNonlocalCount_ == 0`); a per-arm
  #        choice (two locals, or one local + one non-local) is left to the
  #        member-array-storage refusal.  This is what stops a "lift" that
  #        quietly picks one arm of a helper from installing a silent wrong
  #        value.
 #
 # This test rows:
 #   (b)  byte-pinned against the reference (1 insn: MOVR o[COLR].xy, f[TEX0].xy),
 #        which our emitter already produces.
 #   (a)  shape-assertion (>=2 and <=9 insns, presence of the SGT 0.5 test and
 #        the -1.0 constant store) — the reference emits 4 predicated
 #        instructions; we emit 9 (SLE-based select, equivalent result).
 #        Tightening to the reference's predication shape is a separate
 #        codegen concern (dead-store elimination + predicated-store pass
 #        not in scope of this card); a lead with sce-cgc 475 can pin that
 #        shape here the moment parity is lifted.
 #
 # And three SHAPE SAFETY rows the lead required after review, all pinned
 # so a compiler that "lifts" (b) by quietly picking one arm is caught:
 #   pick_two_struct_return_refuse   REFUSAL: a helper that returns TWO struct
 #                                   locals (if (c) return p; return r;) must
 #                                   refuse, never install one arm's storage over
 #                                   the other's.
 #   single_struct_return            VALUE: a helper that returns EXACTLY ONE
 #                                   struct local compiles and copies the tracked
 #                                   storage (mirror[0]=mirror[1]=t.zw here).
  #   compute_wrap_refuse             REFUSAL: a helper whose ARGUMENT is itself a
  #                                   struct-returning call (Plan q = wrap(compute()))
  #                                   must refuse — the argument-inliner must not claim
  #                                   the caller's destination.
  #   pick_with_call_arm_refuse       REFUSAL: a helper whose arm returns a CALL
  #                                   (`if (c) return mk(a); return r;`) is a non-local
  #                                   return; the frame has one base-name (r) AND a
  #                                   non-local, so the install is not done — refuse.
  #   pick_with_param_arm_refuse      REFUSAL: a helper whose arm returns a PARAMETER
  #                                   (`if (c) return x; return r;`) is likewise
  #                                   non-local; refuse.
 set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || { echo "usage: $0 <rsx-cg-compiler>" >&2; exit 2; }
[[ -x "$compiler" ]] || { echo "FAIL: not executable: $compiler" >&2; exit 1; }

shaders="$repo_root/tests/shader-compiler/mossbrook-repros"
decoder="$repo_root/tests/shader-compiler/fp_sources.py"
python="${PYTHON:-python3}"
command -v "$python" >/dev/null 2>&1 || python="python"
command -v "$python" >/dev/null 2>&1 || { echo "FAIL: python interpreter not found" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

accept() {  # <stem> <what>
    local stem="$1"; shift
    set +e
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" \
        "$shaders/$stem.cg" > "$work/$stem.log" 2>&1
    local rc=$?
    set -e
    [[ "$rc" -eq 0 ]] || fail "$*: expected exit 0, got $rc - $(tr -d '\r' < "$work/$stem.log" | grep -m1 -E 'error|nv40' | cut -c1-100)"
    [[ -s "$work/$stem.fpo" ]] || fail "$*: exit 0 but wrote no container"
    echo "  accept ok: $*"
}

# value_rows <stem> <what> <out-lanes> "<IN=v,v,v,v ...> => <v,...>"...
# Runs our container through fp_eval and requires the reference's values; an
# instruction the evaluator does not model is UNJUDGED, and that fails too.
value_rows() {
    local stem="$1" what="$2" lanes="$3"; shift 3
    "$python" - "$repo_root/tests/shader-compiler" "$work/$stem.fpo" "$lanes" "$@" <<'EVAL' \
        || fail "$what: the program does not compute the reference's values (above)"
import sys
sys.path.insert(0, sys.argv[1])
import fp_eval
blob = open(sys.argv[2], "rb").read()
lanes = tuple(int(x) for x in sys.argv[3].split(","))
bad = 0
for row in sys.argv[4:]:
    ins, want = row.split("=>")
    inputs = {k: [float(v) for v in vals.split(",")] for k, vals in (t.split("=") for t in ins.split())}
    want = [float(v) for v in want.split(",")]
    try:
        got = fp_eval.evaluate(blob, inputs, out_lanes=lanes)
    except fp_eval.Unmodelled as e:
        print("  UNJUDGED %s: %s" % (ins.strip(), e))
        bad += 1
        continue
    if got != want:
        print("  WRONG %s: got %s, reference %s" % (ins.strip(), got, want))
        bad += 1
sys.exit(1 if bad else 0)
EVAL
    echo "  value ok: $what ($# input rows, reference values)"
}

# ---------------------------------------------------------------------------
# (b) array-bearing struct copy — byte-pinned against the reference.
# Reference (measured with sce-cgc 475, sce_fp_rsx):
#     MOVR o[COLR].xy, f[TEX0].xy--;
# i.e. ONE instruction: MOV, dst R0 mask xy, src TEXCOORD0 swizzle xxyy, end.
# ---------------------------------------------------------------------------
accept b_member_array_storage_copy \
       "repro b: a helper returning an array-bearing struct is copied to a local"

"$python" "$decoder" "$work/b_member_array_storage_copy.fpo" > "$work/b.u" 2>/dev/null
# One instruction.
insns_b=$(grep -cE '^[0-9]+ ' "$work/b.u" || true)
[[ "$insns_b" == "1" ]] || fail "repro b: expected 1 instruction, got $insns_b"
# MOV, dst R0 mask xy, source TEX0.
# swizzle_text in fp_sources.py decodes each 2-bit field per source slot to
# a letter (not a global swizzle): "xyxx" reads .x in slot 0 and .y in slot 1
# — the .xy the reference emits.  The decoder is not swizzle-notation-
# agnostic; it reports one letter per source slot rather than one for the
# destination, which is why you see 4 letters here even though the write
# target is a 2-component .xy.
grep -qE '^0 MOV dst=R0 mask=xy .* s0=TEX0\.xyxx' "$work/b.u" \
    || fail "repro b: instruction did not match reference MOVR o[COLR].xy f[TEX0].xy — got: $(sed -n '2p' "$work/b.u" | cut -c1-100)"
echo "  byte-pin ok: repro b ucode matches reference (1 insn, MOVR R0.xy <- TEX0.xy)"
value_rows b_member_array_storage_copy "repro b" 0,1 \
    "TEX0=0.25,0.75,-0.5,0.125 => 0.25,0.75"

# ---------------------------------------------------------------------------
# (a) scalar-vector broadcast into a member-array element — shape assertion.
# Reference (sce-cgc 475):
#     SGTRC HC.w, f[TEX1].-x, {0.5}.-x;
#     MOVR   o[COLR].y, {0, 1, 0, 0}.-y--;
#     MOVR   o[COLR].x, {1}.x---;
#     MOVR   o[COLR].x(NE0.w---), {-1}.x---;
# i.e. 4 predicated instructions: a compare-to-0.5, an unconditional 0/1
# store, and a predicated -1 store.
# Ours (current shape): SGT + MOV pair (uninit/else), SGT+MOV pair (then
# branch), a dst=none read-modification-free cond store, and a final
# select-through.  Semantically equivalent (SLE+insert == NE0-predicated
# MOV), but 9 insns.  Tightening to the reference's shape requires a
# predicated-store / dead-store pass that is a separate codegen concern
# (tracked out of this card).
#
# NOTE: fp_sources.py's render() does NOT emit the inline-constant literal
# values — it reports the const SOURCE REGISTER (s1=c0) and its swizzle,
# not the 4 words of float data that follow.  We assert what IS visible:
#   - the instruction count stays in the band [2,9] (a refusal regression
#     emits 0; a "drop the branch" regression emits 1 or worse),
#   - at least one SGT (SLE) test instruction (the `flags > 0.5` branch),
#   - at least one MOV that writes R0 (the final o[COLR] store),
#   - the input (TEX0) is read (the b parameter), and
#   - a CONST source (s1=c0) is used for one of the tests or stores.
# A lead with sce-cgc 475 can tighten any of these once we emit the
# reference's 4-insn predicated shape; pinning the shape here does not
# preclude that later.
# ---------------------------------------------------------------------------
accept a_scalarvec_broadcast_in_memberarray \
       "repro a: scalar broadcast into a member-array under a dynamic loop index"

"$python" "$decoder" "$work/a_scalarvec_broadcast_in_memberarray.fpo" > "$work/a.u" 2>/dev/null
insns_a=$(grep -cE '^[0-9]+ ' "$work/a.u" || true)
[[ "$insns_a" -ge 2 ]] || fail "repro a: expected >=2 instructions, got $insns_a (a refusal regression or a dropped branch)"
[[ "$insns_a" -le 9 ]] || fail "repro a: expected <=9 instructions (current emitter), got $insns_a (emitting MORE code than the current pass)"
# At least one SGT (conditional store) instruction — the `flags > 0.5`
# branch test must exist.
grep -qE '^[0-9]+ SGT dst=R[0-9]' "$work/a.u" \
    || fail "repro a: missing an SGT (flags > 0.5) branch test"
# A final store to R0 (o[COLR]); end=1 marks the terminal instruction of the
# NV40 fragment ucode.
grep -qE '^[0-9]+ MOV dst=R0 .* end=1 ' "$work/a.u" \
    || fail "repro a: missing a final MOV to R0 (o[COLR] output)"
# The input b (TEXCOORD) must be read by at least one instruction.
grep -qE '^[0-9]+ .* s0=TEX0' "$work/a.u" \
    || fail "repro a: input (TEXCOORD) is never read"
# At least one const source — the 0.5 test and 1.0/-1.0 stores use inline
# constants that surface as s1=c0 or s0=c0 in the render.
grep -qE '^[0-9]+ .* s[01]=c[0-9]+' "$work/a.u" \
    || fail "repro a: no inline constant (0.5 test or 1.0/-1.0 store) is used"
echo "  shape ok: repro a ucode has $insns_a insns (SGT test + R0 terminal + TEX0 input + c0 const source); parity to the 4-insn reference is tracked separately"

# VALUE, not shape: run our program through fp_eval on inputs either side of the
# `flags > 0.5` test, the boundary itself included (0.5 must keep +1: SGT, not SGE).
# Expected values are the reference's: its own container, run through the same
# evaluator (sce-cgc 475, 2026-10-16), gives exactly these.
value_rows a_scalarvec_broadcast_in_memberarray "repro a" 0,1 \
    "TEX0=0.25,0.75,0,0 TEX1=0,0,0,0 => 1,1" \
    "TEX0=0.25,0.75,0,0 TEX1=-1,0,0,0 => 1,1" \
    "TEX0=0.25,0.75,0,0 TEX1=0.5,0,0,0 => 1,1" \
    "TEX0=0.25,0.75,0,0 TEX1=0.5009765625,0,0,0 => -1,1" \
    "TEX0=0.25,0.75,0,0 TEX1=1,0,0,0 => -1,1"

# ---------------------------------------------------------------------------
# SHAPE SAFETY rows (required after review).  A compiler that "lifts" (b) by
# quietly picking ONE of the callee's returned arms would pass the two
# single-return rows above and the byte pin, yet emit a silent wrong value
# on a multi-arm helper.  These three pin that the install only happens when
# there is exactly one struct local the callee returns, and never when the
# destination is claimed by an inliner other than the one initialising it.
# ---------------------------------------------------------------------------

refuse_row() {  # <stem> <what>
    local stem="$1"; shift
    set +e
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" \
        "$shaders/$stem.cg" > "$work/$stem.log" 2>&1
    local rc=$?
    set -e
    [[ "$rc" -eq 1 ]] || fail "$*: expected refusal (exit 1), got $rc - exit 0 accepts, and a crash or timeout is not a refusal"
    [[ ! -s "$work/$stem.fpo" ]] || fail "$*: refused but still wrote a container"
    echo "  refuse ok: $*"
}

refuse_saying() {  # <stem> <reason-regex> <what>
    refuse_row "$1" "$3"
    local stem="$1"
    grep -qE "$2" <(tr -d '\r' < "$work/$stem.log") \
        || fail "$3: refused, but not for named reason - wanted /$2/; log: $(tr -d '\r' < "$work/$stem.log" | grep -mi1 -E 'error|refus|nv40|bail' | cut -c1-90)"
    echo "  refuse-saying ok: $3"
}

# (a) two struct locals returned, one per arm: must refuse, never pick an arm.
refuse_saying pick_two_struct_return_refuse \
    "member-array-storage|tracked source" \
    "a helper that returns TWO struct locals (one per arm) is refused, not resolved to one arm"

# (b) exactly one struct local returned: the tracked storage is installed and
#     the copy is skipped; the ucode carries both array elements from `b`.
accept single_struct_return \
       "a helper that returns EXACTLY ONE struct local copies its tracked storage"
"$python" "$decoder" "$work/single_struct_return.fpo" > "$work/s.u" 2>/dev/null
# Two MOVs: mirror[0] = mirror[1] = b = t.zw (the `b` argument).  Both
# write R0 (o[COLR]) and read TEXCOORD0's zw lanes.  Pinning .zw (not .xy)
# is what separates the correct value (b) from a wrong arm (a = t.xy): a
# compiler that silently picked the other arm would emit the .xy lanes and
# be caught.
grep -qE '^[0-9]+ MOV dst=R0 .* s0=TEX0\.zw' "$work/s.u" \
    || fail "single return: expected the tracked storage to be copied from b (t.zw), got the wrong lanes: $(sed -n '2,3p' "$work/s.u" | cut -c1-120)"
value_rows single_struct_return "single return" 0,1,2,3 \
    "TEX0=0.25,0.75,-0.5,0.125 => -0.5,0.125,-0.5,0.125" \
    "TEX0=0.75,0.25,1,-1 => 1,-1,1,-1"
echo "  value ok: single-return helper's tracked storage is copied (mirror[0]=mirror[1]=t.zw)"

# (c) the argument to wrap is itself a struct-returning call: the argument
#     inliner must not claim the caller's `q`; the copy is refused.
refuse_row compute_wrap_refuse \
        "wrap(compute(...)) with compute returning a struct is refused (argument inliner must not claim the caller's destination)"

# (d) a struct return that NAMES A CALL: `if (c) return mk(a); return r;`.
#     The frame built one base-name return (r) AND one non-local (the mk(a)
#     arm).  Remapping r onto `q` would silently install r's mirror[] on top
#     of the mk(a) arm.  The remap is not installed; the copy refuses.
refuse_saying pick_with_call_arm_refuse \
    "member-array-storage|tracked source" \
    "a helper whose arm returns a CALL (return mk(a)) is refused, not resolved to the other local arm"

# (e) a struct return that NAMES A PARAMETER: `if (c) return x; return r;`.
#     Twin of (d): a parameter is not a trackable local, so it is a non-local
#     return; the frame built one base-name (r) AND one non-local (x); refuse.
refuse_saying pick_with_param_arm_refuse \
    "member-array-storage|tracked source" \
    "a helper whose arm returns a PARAMETER (return x) is refused, not resolved to the other local arm"

echo "mossbrook-memberarray: PASS"
