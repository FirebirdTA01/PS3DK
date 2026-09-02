#!/usr/bin/env bash
# Work that FOLLOWS a discard must reach the ucode (t_72810bd7).
#
# THE ASSERTION FLIPPED on 2026-09-02, as the item said it would.  While
# the post-discard lerp was not a shape this path lowered, the required
# behaviour was a REFUSAL - compiling it meant shipping a shader missing a
# line of its source, which is what th06_notex did for months.  The lerp
# lowers now, so the requirement is the stronger one: the shader compiles
# AND the work is there.
#
# "The work is there" is asserted on the UCODE, not on the container's
# input mask.  The mask is not evidence: t_e89cd261 was a defect where the
# mask named a varying no instruction read, so a shader can claim an input
# it never touches.  The lerp reads TEXCOORD2, so some instruction must
# name input source 6 - and if the lerp were dropped again, only the
# colour's TEXCOORD0 would appear.
#
# Two controls, because this fails in both directions: it can miss a drop,
# or it can refuse the whole discard class.  The negative control is a real
# discard shader from the samples.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-discard-completeness-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run() {   # $1 shader path, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$2.fpo" "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    return "$rc"
}

# POSITIVE: post-discard work must compile AND be present in the ucode.
pos="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_discard_then_work_f.cg"
if ! run "$pos" positive; then
    tail -n 10 "$work/positive.log" >&2
    fail "fp_discard_then_work did NOT compile. Its post-discard lerp is a
shape this path lowers now; refusing it is a regression to the behaviour
t_72810bd7 replaced."
fi

# The container run above prints no ucode, so take the dump separately.
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" -p sce_fp_rsx "$pos"
) >"$work/positive-dump.log" 2>&1 ||
    fail "fp_discard_then_work compiled with --emit-container but not without it"

python3 - "$work/positive-dump.log" <<'PY'
import re
import sys

# NV40 fragment INPUT_SRC selector: TEXCOORDn is 4 + n, in word0 bits 13..16.
TEX0, TEX2 = 4, 6
REG_TYPE_INPUT = 1


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


read = set()
for line in open(sys.argv[1], "r", encoding="utf-8"):
    m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
    if not m:
        continue
    w = [unswap(int(x, 16)) for x in m.group(2).split()]
    if len(w) < 4:
        continue
    opcode = (w[0] >> 24) & 0x3F
    if opcode == 0 or opcode > 0x40:
        continue
    if any((w[i] & 3) == REG_TYPE_INPUT for i in (1, 2, 3)):
        read.add((w[0] >> 13) & 0xF)

if not read:
    raise SystemExit(
        "FAIL: no instruction reads a varying at all - the ucode dump did "
        "not parse, so nothing below was actually checked"
    )
if TEX2 not in read:
    raise SystemExit(
        "FAIL: no instruction reads TEXCOORD2 (input source %d); the ucode "
        "names %s.  The fog lerp after the discard reads it, so the work was "
        "dropped - which is the whole of t_72810bd7.  Asserted on the ucode "
        "and not on attributeInputMask, because a container can name an "
        "input no instruction touches (t_e89cd261)."
        % (TEX2, sorted(read))
    )
if TEX0 not in read:
    raise SystemExit(
        "FAIL: no instruction reads TEXCOORD0 (input source %d); the ucode "
        "names %s.  The colour the lerp blends comes from it." % (TEX0, sorted(read))
    )
PY

# NEGATIVE: a discard shader this path lowers correctly -> must still compile.
neg="$repo_root/samples/gcm/hello-ppu-cellgcm-discard-blend/shaders/fpshader.fcg"
if [[ -f "$neg" ]]; then
    if ! run "$neg" negative; then
        tail -n 10 "$work/negative.log" >&2
        fail "the discard-blend sample no longer compiles: the guard is
refusing the whole discard class rather than the dropped-work case"
    fi
else
    fail "negative control shader is missing: $neg"
fi

# ... and its COLOUR must land in R0 (t_5dc260b0).
#
# Compiling is not enough for this shader.  On a fragment program the
# colour output IS R0, and lowerStoreOutput's lane-by-lane branch says so
# only by PINNING the composing writes to register 0 - it emits no output
# store at all.  The allocator treats pins as preferences, so when
# something else held R0 the colour was composed in R1 and nothing ever
# wrote R0: the program computed the right pixel into a register the
# framebuffer does not read.  It compiled, it kept its kills, and it
# painted the wrong picture (rig row fpshader_3d3d33: every painted pixel
# wrong, red collapsed to 2 levels).
#
# The assertion is that THE COLOUR IS NOT A DEAD STORE.  Stating it as
# "after the last KIL the ucode writes all four lanes of R0" was the first
# try and it was wrong: it encodes the REFERENCE's order, which hoists both
# kills above the composition, and our compiler composes first and kills
# last.  That version failed on correct output - including 6ece362's, which
# painted this shader correctly all day - so it would have been a test of
# instruction order wearing the bug's name.
#
# What is actually true of every correct compilation, in any order: R0 is
# the only register the hardware reads after the program ends, so a write
# to any OTHER register whose value is never read again is a value computed
# and thrown away.  In the broken output the four lane writes composing the
# colour land in R1 and nothing reads R1 afterwards - the colour itself is
# the dead store.  That is the defect, and it does not care whether the
# colour is composed in R0 directly or moved there at the end.
#
# Deliberately scoped to THIS shader: dead stores are not illegal in
# general (an unused result the optimiser has not removed is wasteful, not
# wrong).  On this fixture every computed value feeds either a kill or the
# colour, so a dead write means the colour went somewhere the framebuffer
# will not look.
# The container run above prints no ucode, so take the dump separately -
# the same two-step the positive case uses.
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" -p sce_fp_rsx "$neg"
) >"$work/negative-dump.log" 2>&1 ||
    fail "the discard-blend sample compiled with --emit-container but not without it"

python3 - "$work/negative-dump.log" <<'PY'
import re
import sys

# hw[0]: bit 0 end, bits 1..6 destination register, bit 7 destination is an
# H register (two per R slot), bits 9..12 write mask, bit 30 no destination.
KIL = 0x12


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


words = []
for line in open(sys.argv[1], "r", encoding="utf-8"):
    m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
    if m:
        w = [unswap(int(x, 16)) for x in m.group(2).split()]
        if len(w) == 4:
            words.append(w)

if not words:
    raise SystemExit("FAIL: the discard-blend ucode dump did not parse, so "
                     "nothing below was checked")

# Walk instructions, stepping over inline constant blocks - sixteen bytes
# of DATA that must never be decoded as an opcode.
instrs, i = [], 0
while i < len(words):
    w = words[i]
    consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == 2)
    instrs.append(w)
    i += 1 + (1 if consts else 0)

if not any(((w[0] >> 24) & 0x3F) == KIL for w in instrs):
    raise SystemExit("FAIL: the discard-blend sample emits no KIL at all - "
                     "this test's premise is gone, not its assertion")


def dst_slot(w):
    """The R slot an instruction writes, or None when it writes no register."""
    if w[0] & (1 << 30):          # OUT_NONE: a kill, or a cc-only write
        return None
    reg = (w[0] >> 1) & 0x3F
    return (reg >> 1) if (w[0] & (1 << 7)) else reg


def src_slots(w):
    """The R slots an instruction reads."""
    out = set()
    for s in (1, 2, 3):
        if (w[s] & 3) != 0:       # register type 0 is TEMP
            continue
        reg = (w[s] >> 2) & 0x3F
        out.add((reg >> 1) if (w[s] & (1 << 8)) else reg)
    return out


colour = 0
for w in instrs:
    if dst_slot(w) == 0:
        colour |= (w[0] >> 9) & 0xF
if colour != 0xF:
    raise SystemExit(
        "FAIL: the ucode writes R0 lanes 0x%X, not all four.  R0 IS the "
        "colour output on a fragment program, so a lane never written is a "
        "lane the framebuffer reads uninitialised (t_5dc260b0)." % colour)

dead = []
for n, w in enumerate(instrs):
    slot = dst_slot(w)
    if slot is None or slot == 0:
        continue                  # R0 is read by the framebuffer, never dead
    if not any(slot in src_slots(later) for later in instrs[n + 1:]):
        dead.append((n, slot))

if dead:
    raise SystemExit(
        "FAIL: instruction(s) %s write a register nothing reads again.  On "
        "this shader every computed value feeds a kill or the colour, so a "
        "dead write means the colour was composed somewhere the framebuffer "
        "does not read - it was composed in R%d and never moved to R0 "
        "(t_5dc260b0)."
        % (", ".join("%d->R%d" % (n, s) for n, s in dead), dead[0][1]))
PY

printf 'discard-completeness-test: ok\n'
