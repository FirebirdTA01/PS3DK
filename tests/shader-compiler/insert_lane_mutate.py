"""Build the red controls for insert-lane-of-vector-test.sh.

A guard nobody has seen fail is not a guard, so the test doctors the words
the compiler actually emitted and requires the checker to refuse each copy.
Three mutations, each a defect that has either happened or was proposed in
review:

  broadcast   the original t_856689b2 defect: force every component of the
              final colour write's source swizzle to x, so y and z read the
              red channel.
  bypass      codex's review finding against the second version of the
              check: point that same operand at the ORIGINAL INPUT instead
              of the computed temp, keeping the xyz selectors.  Lane
              indices still match; the multiply is gone, and the shader
              paints (.8, .4, .2) where it should paint (.4, .1, .025).
  constant    change one factor in the inline const block, so a lane is
              scaled by the wrong amount while every register, selector and
              opcode stays exactly as the compiler emitted it.

  hwinput     codex's second round: point the MULTIPLY's per-instruction
              input selector (hw[0] bits 13..16 - a fragment instruction has
              ONE) at a different varying, leaving the copy that produces
              lane w on the original.  RGB and w then come from different
              attributes.
  earlyend    codex's third round: set PROGRAM_END on the FIRST
              instruction, so the hardware stops before the multiply and
              before the final colour write.
  saturate    set the multiply's destination saturation bit.
  halfdst     put the multiply's destination in the half bank while the MOV
              that reads it still names R.

Usage: insert_lane_mutate.py <dump> <out> <kind>

Refuses, loudly, if the shape it needs is not in the dump - a control that
silently mutates nothing proves nothing.
"""

import re
import struct
import sys

ROW = re.compile(r"^(\s*)(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$")
CONST_TYPE = 2
INPUT_TYPE = 1
MUL = 0x02
NL = chr(10)


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def words_of(line):
    m = ROW.match(line)
    if not m:
        return None
    return [unswap(int(x, 16)) for x in m.group(3).split()]


def render(line, words):
    m = ROW.match(line)
    return "%s%s:%s" % (m.group(1), m.group(2),
                        "".join(" %08x" % unswap(w) for w in words))


def walk(lines):
    """Yield (line index, words, is_const_block) in program order."""
    i = 0
    while i < len(lines):
        w = words_of(lines[i])
        if w is None or len(w) != 4:
            i += 1
            continue
        has_block = any((w[s] & 3) == CONST_TYPE for s in (1, 2, 3))
        yield i, w, False
        if has_block:
            j = i + 1
            while j < len(lines) and words_of(lines[j]) is None:
                j += 1
            if j < len(lines):
                yield j, words_of(lines[j]), True
                i = j
        i += 1


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: insert_lane_mutate.py <dump> <out> <kind>")
    src, out, kind = sys.argv[1:]
    lines = open(src, "r", encoding="utf-8").read().splitlines()

    instructions = [(i, w) for i, w, block in walk(lines) if not block]
    blocks = [(i, w) for i, w, block in walk(lines) if block]

    target = None
    for i, w in instructions:
        if (w[0] >> 30) & 1:                       # condition-register write
            continue
        if ((w[0] >> 1) & 0x3F) != 0:              # not the colour output
            continue
        if not ((w[0] >> 9) & 0xF) & 0x6:          # does not write y or z
            continue
        target = (i, w)

    if target is None:
        raise SystemExit("no colour-output write to doctor - the red control "
                         "cannot be built, so it would prove nothing")
    ti, tw = target

    producer = None
    producer_at = None
    for i, w in instructions:
        if ((w[0] >> 24) & 0x3F) == MUL and (w[1] & 3) == INPUT_TYPE:
            producer, producer_at = w, i
    needs_producer = kind in ("bypass", "hwinput", "saturate", "halfdst")
    if kind == "earlyend":
        first_at, first = instructions[0]
        first[0] |= 1                              # NVFX_FP_OP_PROGRAM_END
        lines[first_at] = render(lines[first_at], first)
        open(out, "w", encoding="utf-8").write(NL.join(lines) + NL)
        return
    if needs_producer and producer is None:
        raise SystemExit("no multiply reading an input - the shape the %s "
                         "control needs is not in the dump" % kind)

    if kind == "broadcast":
        tw[1] &= ~(0xFF << 9)                      # every component reads x
    elif kind == "bypass":
        keep = tw[1] & ~0xFF                       # swizzle, negate, half
        tw[1] = keep | (producer[1] & 0xFF)        # its type and register
        # The input register's identity is per-INSTRUCTION, hw[0] bits
        # 13..16, so pointing the operand at an input without carrying that
        # selector would read whatever varying this instruction names.
        tw[0] = (tw[0] & ~(0xF << 13)) | (producer[0] & (0xF << 13))
    elif kind == "hwinput":
        current = (producer[0] >> 13) & 0xF
        producer[0] = (producer[0] & ~(0xF << 13)) | (((current + 1) & 0xF) << 13)
        lines[producer_at] = render(lines[producer_at], producer)
        open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        return
    elif kind in ("saturate", "halfdst"):
        producer[0] |= (1 << 31) if kind == "saturate" else (1 << 7)
        lines[producer_at] = render(lines[producer_at], producer)
        open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        return
    elif kind == "constant":
        if not blocks:
            raise SystemExit("no inline const block to alter")
        bi, bw = blocks[-1]
        value = struct.unpack("<f", struct.pack("<I", bw[1]))[0]
        bw[1] = struct.unpack("<I", struct.pack("<f", value * 2.0))[0]
        lines[bi] = render(lines[bi], bw)
        open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        return
    else:
        raise SystemExit("unknown mutation %r" % kind)

    lines[ti] = render(lines[ti], tw)
    open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
