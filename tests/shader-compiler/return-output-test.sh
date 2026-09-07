#!/usr/bin/env bash
# t_5c12df56: what a fragment entry RETURNS must reach the ucode with the
# same fidelity as what it writes through an out parameter.
#
# Two independent defects, both silent (exit 0, container written, no
# diagnostic), both reproduced by `return 1.0;`:
#
#   A. a scalar returned into a float4 output is not broadcast.  The write
#      mask is 0x1, so three of the four colour lanes are never written.
#   B. a half RETURN TYPE is not detected at all.  The t_80dad2dd detection
#      walks the entry's parameters, so `half4 main() : COLOR` keeps
#      outputFromH0 clear and emits a full-precision MOVR.
#
# The words below are the REFERENCE's, decoded from its own container for
# these exact sources; the expectations are not derived from our encoder.
# The container stores each ucode word with its 16-bit halves swapped, so
# the raw file words are pinned as-is and the mask/precision fields are
# read through the tree's decoder, which also skips inline constant
# blocks - see ucode_decode.py's header for the field positions.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-return-output-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

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
    ) >"$work/$stem.log" 2>"$work/$stem.err" || {
        tail -n 20 "$work/$stem.err" >&2
        fail "$stem did not compile"
    }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
}

emit fp_return_scalar_f
emit fp_return_half_f
emit fp_return_scalar_control_f
emit fp_return_half_depth_control_f
emit fp_return_half_with_depth_f

python3 - "$repo_root/tests/shader-compiler" \
          "$work/fp_return_scalar_f.fpo" "$work/fp_return_half_f.fpo" \
          "$work/fp_return_scalar_control_f.fpo" \
          "$work/fp_return_half_depth_control_f.fpo" \
          "$work/fp_return_half_with_depth_f.fpo" <<'PY'
import struct
import sys
import tempfile

# The instruction walk comes from the tree's decoder, not from a second
# reading of the encoding: an inline constant block is four words of DATA
# following the instruction that names them, and a hand-rolled "every
# fourth word is an opcode" walk decodes those constants as instructions.
sys.path.insert(0, sys.argv[1])
from ucode_decode import decode  # noqa: E402


def container(path):
    b = open(path, "rb").read()
    _, _, _, _, _, prog, ucode_size, ucode = struct.unpack_from(">8I", b, 0)
    regs, h0, depth, kill = struct.unpack_from(">4B", b, prog + 18)
    words = list(struct.unpack_from(">%dI" % (ucode_size // 4), b, ucode))
    # decode() reads the dump format the shader tests already print.
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
        for i in range(0, len(words), 4):
            fh.write("%d: %s\n"
                     % (i // 4, " ".join("%08x" % w for w in words[i:i + 4])))
        dump = fh.name
    instructions = list(decode(dump))
    for d in instructions:
        d["word0"] = words[4 * d["index"]]
    return {"regs": regs, "h0": h0, "depth": depth, "kill": kill,
            "words": words, "instructions": instructions}


FP16 = 1

scalar = container(sys.argv[2])
half = container(sys.argv[3])
control = container(sys.argv[4])
half_depth = container(sys.argv[5])
half_with_depth = container(sys.argv[6])

problems = []

# (A) The reference's whole program for `float4 main() : COLOR
# { return 1.0; }`.  Only the second instruction's word 0 differs on the
# general path today, and only in the write mask, so the full list is a
# safe pin: if anything else moves, the diff names it.
REFERENCE_SCALAR = [
    0x1E7E7E00, 0xC8001C9D, 0xC8000001, 0xC8000001,
    0x1E010100, 0x00021C9C, 0xC8000001, 0xC8000001,
    0x00003F80, 0x00000000, 0x00000000, 0x00000000,
]
if scalar["words"] != REFERENCE_SCALAR:
    diff = [(i, "0x%08x" % a, "0x%08x" % b)
            for i, (a, b) in enumerate(zip(scalar["words"], REFERENCE_SCALAR))
            if a != b]
    problems.append(
        "returned scalar: ucode differs from the reference at %s "
        "(ours, reference); %d words vs %d"
        % (diff or "no shared index", len(scalar["words"]),
           len(REFERENCE_SCALAR)))
colour = [d for d in scalar["instructions"] if d["end"]]
if colour and colour[0]["mask"] != 0xF:
    problems.append(
        "returned scalar: the colour write mask is 0x%X, not 0xF - a scalar "
        "returned into a float4 output must broadcast, and the lanes outside "
        "the mask keep whatever the register held" % colour[0]["mask"])
if scalar["h0"]:
    problems.append("the float return set outputFromH0=%d" % scalar["h0"])

# (B) The half return type.  The reference's container for this source is
# one MOVH and no more; ours still emits a leading FENCBR for every half
# output, which is a SEPARATE divergence the out-parameter path shows too,
# so this half pins properties rather than the word list: the flag, and a
# colour write that is half-precision and full-mask.
if half["h0"] != 1:
    problems.append(
        "`half4 main() : COLOR` produced outputFromH0=%d: the declared "
        "return type never reached the container, so the colour is left in "
        "R0 at full precision instead of the H0 the source asked for"
        % half["h0"])

movh = [d for d in half["instructions"] if d["prec"] == FP16]
if not movh:
    problems.append(
        "the half return emitted no fp16 instruction; decoded instructions "
        "are %s" % ", ".join("0x%08x (prec %d, mask 0x%X)"
                             % (d["word0"], d["prec"], d["mask"])
                             for d in half["instructions"]))
else:
    if movh[0]["mask"] != 0xF:
        problems.append(
            "half return: the colour write mask is 0x%X, not 0xF"
            % movh[0]["mask"])
    # The reference's MOVH for this source, exactly.
    if movh[0]["word0"] != 0x1E810140:
        problems.append(
            "half return: colour word0 is 0x%08x, expected the reference's "
            "0x1E810140 (XOR 0x%08x)"
            % (movh[0]["word0"], movh[0]["word0"] ^ 0x1E810140))
if half["depth"] or half["kill"]:
    problems.append("half return fixture declares no DEPTH and no discard, "
                    "but the container reports depthReplace=%d pixelKill=%d"
                    % (half["depth"], half["kill"]))

# The control is byte-identical to the reference today and must stay so: a
# fix that broadcasts every scalar store, or sets the half flag whenever an
# entry has a return type, moves it.
if control["words"] != REFERENCE_SCALAR:
    problems.append(
        "the out-parameter control moved: it was byte-identical to the "
        "reference and is the negative half of this pair")
if control["h0"]:
    problems.append("the out-parameter float control set outputFromH0=%d"
                    % control["h0"])

# The second control: a half return that is NOT the colour.  A COLOR0 store
# an out parameter owns belongs to that parameter whatever the entry
# returns, so this shader's colour stays fp32 in R0.  Reading the return
# type without that exclusion turned it into H0.
if half_depth["h0"]:
    problems.append(
        "`half main(out float4 colour : COLOR) : DEPTH` set outputFromH0=%d: "
        "the half return is the DEPTH export, and the colour belongs to the "
        "out parameter that binds COLOR0" % half_depth["h0"])
if not half_depth["depth"]:
    problems.append("the half-depth control reports depthReplace=0; the "
                    "fixture no longer exports depth and proves nothing")
# The colour write is the full-mask one; the depth export is masked to
# R1.z (0x4) in the reference and in ours, so this stays true if the depth
# store's precision is ever brought in line with the reference's fp16.  No
# word list is pinned: we are not reference-identical on this shape yet
# (two extra FENCBRs and an fp32 depth store), and those are other rows.
half_colour_writes = [d for d in half_depth["instructions"]
                      if d["mask"] == 0xF and d["prec"] == FP16]
if half_colour_writes:
    problems.append(
        "the half-depth control has a full-mask fp16 write (0x%08x): its "
        "colour is declared float4 and must stay full precision"
        % half_colour_writes[0]["word0"])

# The mirror of the control: the HALF is the colour and the out parameter
# is the depth.  No out parameter binds COLOR0, so the return type decides.
# The two fixtures differ only in which output is half, so a rule reading
# the return type alone sets the flag on both and a rule reading parameters
# alone sets it on neither.
if half_with_depth["h0"] != 1:
    problems.append(
        "`half4 main(out float d : DEPTH) : COLOR` produced outputFromH0=%d: "
        "no out parameter binds COLOR0 here, so the half return type is what "
        "declares the colour" % half_with_depth["h0"])
if not half_with_depth["depth"]:
    problems.append("the half-colour/float-depth fixture reports "
                    "depthReplace=0 and no longer exports depth")
mixed_colour = [d for d in half_with_depth["instructions"] if d["prec"] == FP16]
if not mixed_colour:
    problems.append(
        "the half-colour/float-depth fixture emitted no fp16 instruction: "
        "decoded %s" % ", ".join("0x%08x (prec %d, mask 0x%X)"
                                 % (d["word0"], d["prec"], d["mask"])
                                 for d in half_with_depth["instructions"]))
elif mixed_colour[0]["word0"] != 0x1E800140:
    problems.append(
        "half-colour/float-depth: colour word0 is 0x%08x, expected the "
        "reference's 0x1E800140 (XOR 0x%08x)"
        % (mixed_colour[0]["word0"], mixed_colour[0]["word0"] ^ 0x1E800140))

if problems:
    for problem in problems:
        print("FAIL: " + problem)
    raise SystemExit(1)

print("return-output-test: ok (returned scalar broadcasts to the reference's "
      "ucode; a half return type sets outputFromH0 and emits fp16; the "
      "out-parameter controls are unmoved)")
PY

printf 'PASS: return-output-test\n'
