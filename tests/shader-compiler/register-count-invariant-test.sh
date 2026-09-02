#!/usr/bin/env bash
# The container's registerCount must be the highest R slot the ucode
# actually writes, plus one (t_5dc260b0).
#
# registerCount is a HARDWARE ALLOCATION: the RSX is told how many temp
# registers to reserve for the program.  Nothing in the pipeline checks it
# against the code, and RPCS3 does not enforce it, so BOTH errors are
# invisible to the pixel rig:
#
#   declared too LOW   the program writes a register it was not given.  On
#                      the emulator it reads identical; on the console it
#                      paints garbage.
#   declared too HIGH  the program is refused by the fragment budget for
#                      registers it never touches.  That is the defect this
#                      test exists for: the fragment allocator used to
#                      number its bank from the count of virtual
#                      DEFINITIONS, so a program that held 18 values at a
#                      time declared 62 and was refused at 48.
#
# So the check is an EQUALITY, not a bound, and it is read from the emitted
# BYTES on both sides - the ucode words for the slots, the container header
# for the count - rather than from a diagnostic or a disassembler.
#
# The floor of 2 is the reference compiler's minimum (nv40_emit.h); a
# program that writes nothing above R1 still declares 2.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-register-count-invariant-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# The regression fixture, generated rather than checked in: sixteen
# independent terms summed into the output.  It DEFINES far more values
# than it holds - which is the shape that made the numbering defect
# refuse - and the reference compiles it declaring 4 registers.
lanes=(x y z w)
{
    printf 'void main(float4 c : TEXCOORD0, float4 d : TEXCOORD1, out float4 o : COLOR)\n{\n'
    for i in $(seq 0 15); do
        printf '    float4 t%d = c * %d.125 + d.%s * 0.%d;\n' \
            "$i" "$((i + 1))" "${lanes[$((i % 4))]}" "$((i + 3))"
    done
    printf '    o = t0'
    for i in $(seq 1 15); do printf ' + t%d' "$i"; done
    printf ';\n}\n'
} > "$work/n16.fcg"

compile() {   # $1 source path, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-30s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$2.fpo" "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 did not compile"
    fi
}

# The generated fixture first: before the numbering fix this refused with
# "program needs 62 temp registers", so its mere compilation is the
# acceptance half of the regression.
compile "$work/n16.fcg" n16

for stem in fp_normalized_phong_vecinsert_f fp_computed_color_store_f \
            fp_half_cast_f fp_dot4_width_f; do
    [[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"
    compile "$shaders/$stem.cg" "$stem"
done

cat >"$work/check.py" <<'PY'
import struct
import sys

# CgBinaryProgram header, all u32 big-endian:
#   0 profile, 4 revision, 8 totalSize, 12 parameterCount,
#   16 parameterArray, 20 program, 24 ucodeSize, 28 ucode
#
# CgBinaryFragmentProgram at header[program]:
#   0 instructionCount, 4 attributeInputMask, 8 partialTexType,
#   12 texCoordsInputMask (u16), 14 texCoords2D (u16),
#   16 texCoordsCentroid (u16), 18 registerCount (u8), ...
PROGRAM_OFF, UCODE_SIZE_OFF, UCODE_OFF = 20, 24, 28
REGISTER_COUNT_IN_PROGRAM = 18

# hw[0]: bit 0 end, bits 1..6 destination register, bit 7 destination is an
# H (fp16) register - two of which share one R slot - bit 30 no destination.
DST_SHIFT, DST_MASK = 1, 0x3F
HALF_BIT = 1 << 7
NONE_BIT = 1 << 30

FLOOR = 2   # the reference compiler's minimum, nv40_emit.h


def be32(blob, off):
    return struct.unpack_from(">I", blob, off)[0]


def unswap(v):
    """On disk the two halfwords of each ucode word are swapped."""
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


bad = []
for path in sys.argv[1:]:
    blob = open(path, "rb").read()
    program = be32(blob, PROGRAM_OFF)
    declared = blob[program + REGISTER_COUNT_IN_PROGRAM]
    ucode_off, ucode_size = be32(blob, UCODE_OFF), be32(blob, UCODE_SIZE_OFF)

    highest = -1
    i = 0
    words = ucode_size // 4
    while i < words:
        w = [unswap(be32(blob, ucode_off + (i + k) * 4)) for k in range(4)]
        # A source of register type CONST (2) names an inline constant
        # block: sixteen bytes of DATA following this instruction, which
        # must not be walked as an opcode.
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == 2)
        if not (w[0] & NONE_BIT):
            reg = (w[0] >> DST_SHIFT) & DST_MASK
            slot = (reg >> 1) if (w[0] & HALF_BIT) else reg
            highest = max(highest, slot)
        i += 4 * (1 + (1 if consts else 0))

    expected = max(FLOOR, highest + 1)
    if declared != expected:
        bad.append("%s: declared registerCount %d, highest R slot written %d "
                   "(expected %d)" % (path.split("/")[-1], declared, highest,
                                      expected))
    if declared >= 48:
        bad.append("%s: declared registerCount %d is at or past the fragment "
                   "budget of 48" % (path.split("/")[-1], declared))

if bad:
    for line in bad:
        sys.stderr.write("FAIL: " + line + "\n")
    sys.exit(1)
print("registerCount == highest written R slot + 1 on %d containers"
      % (len(sys.argv) - 1))
PY

# SELF-CONTROL, before the checker is believed about anything.
#
# The invariant HELD on the binary this defect was found on - emitDst has
# always derived the count from the highest slot written, and the defect
# was the slot being high - so "the test fails on the old compiler" only
# ever exercised the n16 refusal, never the checker.  A wrong container
# offset or a mis-walked const block would sail through that.
#
# So mutate the byte the checker reads, one up and one down, and require
# it to object to each.  Down is the direction that paints garbage on the
# console and reads identical on RPCS3; up is this defect's direction.
mutate() {   # $1 source container, $2 delta, $3 output
    python3 - "$1" "$2" "$3" <<'PY'
import struct
import sys

src, delta, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3]
blob = bytearray(open(src, "rb").read())
program = struct.unpack_from(">I", blob, 20)[0]
at = program + 18
blob[at] = (blob[at] + delta) & 0xFF
open(dst, "wb").write(blob)
print("mutated registerCount %+d at container offset %d" % (delta, at))
PY
}

for delta in 1 -1; do
    if [[ "$delta" -gt 0 ]]; then tag=mut_up; else tag=mut_down; fi
    mutate "$work/n16.fpo" "$delta" "$work/$tag.fpo" >/dev/null
    rc=0
    python3 "$work/check.py" "$work/$tag.fpo" >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -ne 0 ]] || fail "checker accepted a registerCount mutated by $delta - it is not reading the field it claims to"
    grep -q "declared registerCount" "$work/$tag.log" \
        || fail "checker rejected the $delta mutation without naming the declared count"
done

# ... and only now on the real containers.
python3 "$work/check.py" "$work"/n16.fpo "$work"/fp_*.fpo

printf 'PASS: register-count-invariant-test\n'
