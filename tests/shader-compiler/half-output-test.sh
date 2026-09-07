#!/usr/bin/env bash
# t_80dad2dd: a DECLARED half fragment output must reach the container.
#
# `out half4 o : COLOR` selects a different hardware output register (H0
# rather than R0) at a different precision, and the container records that in
# outputFromH0.  The runtime reads that flag to decide which register the
# colour is taken from.  Dropping the flag AND emitting MOVR is internally
# consistent - the runtime reads R0 and R0 was written - so this is not a
# read of an unwritten register; it is the declared output type being
# disregarded, so the program honours neither the register nor the
# precision the source asked for (t_5c12df56 corrected this wording).
#
# NO CONTROL-WORD VALUE APPEARS IN THIS TEST, deliberately.  Which bits the
# bind sets is the SDK's business and has been measured and corrected once
# already; a compiler fixture that names those numbers would be invalidated
# by the next runtime measurement even though nothing about the compiler
# changed.  What this test owns is the CONTAINER: the flag and the ucode.
#
# The general path (the default) drops all three: it emits MOVR o[COLR] with
# the flag clear, exit 0, container written, no diagnostic.  The retired
# --legacy-lowering path gets it right and is byte-identical to the reference
# on this shader, which is where the expected words below come from.
#
# CONTROL: fp_float_output_f.cg is the same source with `out float4`.  It must
# keep MOVR o[COLR] and a clear flag, so a fix that sets the bit
# unconditionally fails here rather than passing quietly.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# The refusal_status helper that stood here (exit 1 EXACTLY - a 124 timeout
# and a >=128 signal both satisfy "did not exit 0" while meaning the
# compiler never reached the decision, t_fd95d1b9) went with the partial
# half output's refusal below.  This file no longer asserts any refusal, so
# keeping an unused copy would be dead code; the same helper is still in
# every test that does assert one.

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-half-output-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

emit() {
    local stem="$1"
    local src="$shaders/$stem.cg"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$src"
    ) >"$work/$stem.log" 2>&1 || {
        tail -n 20 "$work/$stem.log" >&2
        fail "$stem did not compile"
    }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem wrote no container"
}

emit fp_half_output_f
emit fp_float_output_f

# THE PARTIAL HALF OUTPUT'S REFUSAL IS GONE, as the version of this block
# that stood here said it would be: "when it is lifted this check turns red
# and names the shape".  It was lifted by deciding half-ness in the builder
# instead of after allocation, so the lane-by-lane composition writes H0
# directly rather than being composed in an fp32 temp pinned to the output
# slot.  That shape's assertions now live in h0-alias-test.sh, which owns
# it properly - per-instruction precision and lane coverage, not just a
# status - and this file keeps the one-variable half/float pair it was
# built around.  Nothing is asserted about fp_half_output_partial_f here,
# so the two tests do not both claim the same property.

python3 - "$work/fp_half_output_f.fpo" "$work/fp_float_output_f.fpo" <<'PY'
import struct
import sys

# CgBinaryProgram: profile, revision, totalSize, parameterCount,
# parameterArray, program, ucodeSize, ucode - all big-endian.  The fragment
# header at `program` ends with registerCount, outputFromH0, depthReplace,
# pixelKill, one byte each, so the flag is at program + 19.
def container(path):
    b = open(path, "rb").read()
    _, _, _, _, _, prog, ucode_size, ucode = struct.unpack_from(">8I", b, 0)
    regs, h0, depth, kill = struct.unpack_from(">4B", b, prog + 18)
    words = struct.unpack_from(">%dI" % (ucode_size // 4), b, ucode)
    return {"h0": h0, "depth": depth, "kill": kill, "regs": regs,
            "words": words}

half = container(sys.argv[1])
flt = container(sys.argv[2])

problems = []

# The reference's bytes for this shader, taken from the container the retired
# path still produces byte-identically: MOVH into the half output carries
# OUT_REG_HALF (0x00800000) and fp16 precision (0x00000040) in word 0.
HALF_WORD0 = 0x9E810140
FLOAT_WORD0 = 0x9E010100

if half["h0"] != 1:
    problems.append(
        "declared `out half4` produced outputFromH0=%d: the container does not "
        "record the half output, so the colour stays in R0 at full precision "
        "instead of the H0 the source asked for"
        % half["h0"])
if half["words"][0] != HALF_WORD0:
    problems.append(
        "half output word0 is 0x%08x, expected 0x%08x (XOR 0x%08x): "
        "OUT_REG_HALF is 0x00800000 and fp16 precision is 0x00000040"
        % (half["words"][0], HALF_WORD0, half["words"][0] ^ HALF_WORD0))
if half["depth"] or half["kill"]:
    problems.append("half fixture declares no DEPTH and no discard, but the "
                    "container reports depthReplace=%d pixelKill=%d"
                    % (half["depth"], half["kill"]))

# The control must not move.
if flt["h0"] != 0:
    problems.append("the float control set outputFromH0=%d: a fix that sets "
                    "the flag unconditionally is not the fix" % flt["h0"])
if flt["words"][0] != FLOAT_WORD0:
    problems.append("float control word0 is 0x%08x, expected 0x%08x - the "
                    "control moved" % (flt["words"][0], FLOAT_WORD0))

# The pair differs in the output register and its precision, nothing else.
if len(half["words"]) == len(flt["words"]):
    diff = [i for i, (a, b) in enumerate(zip(half["words"], flt["words"]))
            if a != b]
    if not diff:
        problems.append("the half and float containers are byte-identical in "
                        "ucode: `out half4` and `out float4` compile to the "
                        "same program, so the declared type reached nothing")
    elif diff != [0]:
        problems.append("half and float containers differ in ucode words %s; "
                        "only word 0 (the output register and its precision) "
                        "should differ" % diff)
else:
    problems.append("half and float containers have different ucode lengths "
                    "(%d vs %d): the pair is no longer one-variable"
                    % (len(half["words"]), len(flt["words"])))

if problems:
    for p in problems:
        print("FAIL: %s" % p)
    raise SystemExit(1)

print("half-output-test: outputFromH0 set, word0 0x%08x, control unmoved"
      % half["words"][0])
PY

printf 'half-output-test: PASS\n'
