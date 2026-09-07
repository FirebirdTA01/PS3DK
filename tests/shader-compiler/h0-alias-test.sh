#!/usr/bin/env bash
# t_80dad2dd: a declared half colour output must reach H0 THROUGH THE
# ALLOCATOR, not by a patch applied after it.
#
# H0 is the low half of R0.  The first version of this feature decided
# half-ness in the emitter, after allocation, and stamped the precision onto
# `dst.output` instructions - which two of lowerStoreOutput's three colour
# folds never produce, because they carry "this value is the colour" in an
# outputPin on a temp instead.  On those folds the colour was composed in
# fp32 R0 and shipped with outputFromH0 clear, so `half4` and `float4` entry
# points compiled to the same container.  What stood in the way of that
# being noticed was a blanket refusal of any program with a temp in R0,
# which refused 45 of the reference SDK's fragment programs - among them the
# shape THE REFERENCE ITSELF EMITS: `TEXR R0.xyz, f[TEX0], TEX0` followed by
# `MOVH H0.xyz, R0`, where the temp's last read is the instruction that
# writes the colour.
#
# So the property under test is not "R0 is kept clear".  It is:
#
#   1. a half colour output whose value passes through a temp in R0
#      COMPILES, and its colour is fp16;
#   2. a half colour output composed LANE BY LANE - the fold that emits no
#      dst.output instruction at all - is fp16 in every lane, with all four
#      lanes written;
#   3. an `out float4` colour still wins over a half RETURN type, so the
#      classifier did not become "anything half anywhere";
#   4. the float control is untouched.
#
# NO CONTROL-WORD VALUE APPEARS HERE.  Which bits the runtime bind sets is
# the SDK's business and has moved once already (t_96daf53b); this test owns
# the container flag and the decoded instructions.
#
# The instructions are decoded with the shared ucode_decode, not read out of
# the disassembler's text: a container field is not evidence about the
# ucode.  `prec` is the discriminator - 1 is fp16 - and it is asserted on
# EVERY instruction that writes the colour, because a program that is half
# in its last write and fp32 in an earlier one paints the earlier lanes at
# the wrong precision while still reporting outputFromH0=1.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-h0-alias-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# Compile one fixture BOTH ways: the container (for the flag) and the text
# dump (for the instructions).  A fixture that refuses here is the
# regression this file exists to catch, so the failure names the shape
# rather than surfacing later as a missing file.
# The ucode dump is on stdout and the diagnostics are on stderr; merging
# them lets a stderr line land INSIDE a hex row, which costs the row,
# shifts every later one and decodes a constant as an instruction writing
# a register nothing reads (the false R33, 2026-09-07).  Keep them apart;
# the decoder's refusal is the fallback, not the fix.
emit() {
    local stem="$1"
    local src="$shaders/$stem.cg"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$src"
    ) >"$work/$stem.clog" 2>"$work/$stem.cerr" || {
        tail -n 20 "$work/$stem.cerr" >&2
        fail "$stem did not compile; a declared half colour output must not refuse merely for holding a temp in R0 (t_80dad2dd)"
    }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$work/$stem.log" 2>"$work/$stem.err" || {
        tail -n 20 "$work/$stem.err" >&2
        fail "$stem did not compile on the dump path"
    }
}

emit fp_half_output_scratch_f
emit fp_half_output_partial_f
emit fp_half_output_merged_return_f
emit fp_half_output_shared_value_f
emit fp_half_return_float_colour_f
emit fp_float_output_f

python3 - "$repo_root/tests/shader-compiler" "$work" <<'PY'
import struct
import sys

sys.path.insert(0, sys.argv[1])
from ucode_decode import decode

work = sys.argv[2]

# CgBinaryProgram: profile, revision, totalSize, parameterCount,
# parameterArray, program, ucodeSize, ucode - all big-endian.  The fragment
# header at `program` ends with registerCount, outputFromH0, depthReplace,
# pixelKill, one byte each.
def container(stem):
    b = open("%s/%s.fpo" % (work, stem), "rb").read()
    _, _, _, _, _, prog, _, _ = struct.unpack_from(">8I", b, 0)
    regs, h0, depth, kill = struct.unpack_from(">4B", b, prog + 18)
    return {"regs": regs, "h0": h0, "depth": depth, "kill": kill}

# decode() is the shared walk, used rather than a local read of the words
# for two reasons measured while writing this test: groups() has ALREADY
# unswapped the halfwords, so unswapping again silently returns the raw word
# and every field lands in the wrong bits; and an inline constant block is a
# 16-byte GROUP that is not an instruction, which decode() steps over using
# the source-type count.  A local walk reported five instructions for a
# four-instruction program and read every fp16 write as fp32.
def instrs(stem):
    return list(decode("%s/%s.log" % (work, stem)))

problems = []

# --- 1. the scratch shape: the 45-row bucket ------------------------------
scratch = container("fp_half_output_scratch_f")
if scratch["h0"] != 1:
    problems.append(
        "fp_half_output_scratch_f reports outputFromH0=%d: a half colour "
        "output whose value passes through a temp lost its H0 register, so "
        "the runtime takes the colour from fp32 R0" % scratch["h0"])

# --- 2. the lane-by-lane fold ---------------------------------------------
# The fold that emits NO dst.output instruction.  Its colour writers are
# pinned temps, so a stamp reaching only dst.output instructions leaves
# every one of them fp32; the precision is therefore asserted per
# instruction, and the lane coverage with it.
partial = container("fp_half_output_partial_f")
if partial["h0"] != 1:
    problems.append(
        "fp_half_output_partial_f reports outputFromH0=%d: the lane-by-lane "
        "colour fold did not reach H0" % partial["h0"])

part_i = [i for i in instrs("fp_half_output_partial_f") if not i["none"]]
if not part_i:
    problems.append("fp_half_output_partial_f decoded to no instructions")
else:
    fp32 = [n for n, i in enumerate(part_i) if i["prec"] != 1]
    if fp32:
        problems.append(
            "fp_half_output_partial_f writes the colour at fp32 in "
            "instruction(s) %s of %d: a colour half in one lane and full in "
            "another paints the full lanes at the wrong precision while the "
            "container still reports outputFromH0=1" % (fp32, len(part_i)))
    lanes = 0
    for i in part_i:
        lanes |= i["mask"]
    if lanes != 0xF:
        problems.append(
            "fp_half_output_partial_f writes lanes 0x%x of the colour, not "
            "0xF: a lane the composition dropped is whatever was already in "
            "H0" % lanes)

# --- 2b. the store lowerStoreOutput never sees ----------------------------
# A merged conditional return builds its own colour store inside
# lowerMergedReturnSelect, so a stamp applied only in lowerStoreOutput's
# folds leaves it fp32.  This row exists because that hole was real and was
# caught in review, not in testing: the shader ACCEPTED with the colour in
# R0 and the flag clear, which is a quieter failure than the refusal it
# replaced.
merged = container("fp_half_output_merged_return_f")
if merged["h0"] != 1:
    problems.append(
        "fp_half_output_merged_return_f reports outputFromH0=%d: the colour "
        "store built by lowerMergedReturnSelect did not get the half stamp, "
        "so a half4 conditional return paints from fp32 R0 while claiming "
        "nothing is wrong" % merged["h0"])
merged_i = [i for i in instrs("fp_half_output_merged_return_f")
            if not i["none"]]
if merged_i and merged_i[-1]["prec"] != 1:
    problems.append(
        "fp_half_output_merged_return_f writes its colour at precision %d: "
        "the merged store must land in H0 at fp16" % merged_i[-1]["prec"])

# --- 2c. a value the colour SHARES with another consumer ------------------
# Stamping a colour's PRODUCERS half changes the precision of the value, not
# just of the colour taken from it.  When something else reads that value -
# here the depth export - it must still read full precision, so the
# conversion belongs AT the colour store and nowhere earlier.  The
# discriminating count is how many instructions are fp16: the reference
# emits exactly one (the store), and stamping the producers instead made it
# two while the depth quietly read H0.x.
shared = container("fp_half_output_shared_value_f")
if shared["h0"] != 1:
    problems.append(
        "fp_half_output_shared_value_f reports outputFromH0=%d: the colour "
        "is still declared half and must still reach H0" % shared["h0"])
if shared["depth"] != 1:
    problems.append(
        "fp_half_output_shared_value_f reports depthReplace=%d: the fixture "
        "exports depth, and without it there is no second consumer and the "
        "row tests nothing" % shared["depth"])
shared_i = [i for i in instrs("fp_half_output_shared_value_f")
            if not i["none"]]
half_writes = [n for n, i in enumerate(shared_i) if i["prec"] == 1]
if len(half_writes) != 1:
    problems.append(
        "fp_half_output_shared_value_f emits %d fp16 instructions (%s) of "
        "%d, expected exactly one - the colour store.  More than one means "
        "the shared value was itself computed at half precision, so the "
        "float DEPTH export reads a half-rounded number while the container "
        "reports nothing wrong"
        % (len(half_writes), half_writes, len(shared_i)))
elif shared_i[half_writes[0]]["dst"] != 0:
    problems.append(
        "fp_half_output_shared_value_f's only fp16 instruction writes "
        "register %d, not the colour register 0"
        % shared_i[half_writes[0]]["dst"])

# --- 3. the out-parameter wins over a half return -------------------------
mixed = container("fp_half_return_float_colour_f")
if mixed["h0"] != 0:
    problems.append(
        "fp_half_return_float_colour_f reports outputFromH0=%d: the COLOR0 "
        "output is a declared `out float4` and must stay in R0 at full "
        "precision; the half RETURN belongs to the DEPTH export.  A "
        "classifier reading `half return type AND some COLOR0 store exists` "
        "turns this shader's fp32 colour into H0" % mixed["h0"])
if mixed["depth"] != 1:
    problems.append(
        "fp_half_return_float_colour_f reports depthReplace=%d: the control "
        "no longer exports depth, so it is no longer testing the rule it "
        "was built for" % mixed["depth"])

# --- 4. the float control -------------------------------------------------
flt = container("fp_float_output_f")
if flt["h0"] != 0:
    problems.append(
        "the float control set outputFromH0=%d: a fix that stamps the flag "
        "unconditionally is not the fix" % flt["h0"])
for n, i in enumerate(instrs("fp_float_output_f")):
    if i["none"]:
        continue
    if i["prec"] != 0:
        problems.append(
            "the float control emits instruction %d at precision %d: the "
            "half stamp reached a program with no half output"
            % (n, i["prec"]))

if problems:
    for p in problems:
        print("FAIL: %s" % p)
    raise SystemExit(1)

print("h0-alias-test: scratch h0=1; lane-by-lane fold fp16 on %d "
      "instructions covering all four lanes; merged conditional return "
      "h0=1; shared value converts in exactly one instruction so the depth "
      "export stays full precision; out-float colour stayed in R0 with "
      "depthReplace=1; float control unmoved" % len(part_i))
PY

printf 'h0-alias-test: PASS\n'
