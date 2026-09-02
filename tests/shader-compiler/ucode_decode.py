"""Print one decoded line per NV40 fragment instruction in a ucode dump.

Shared by the shader-compiler shell tests that need to assert on the
INSTRUCTIONS rather than on a container field or a diagnostic.  A field a
container declares is not evidence about the ucode - t_e89cd261 was a
defect where the input mask named a varying no instruction read - so the
tests that matter decode the words.

Field positions are taken from nvfx_shader.h, not from a reading of the
disassembler's output:

    hw[0] bit 0       program end
    hw[0] bits 1..6   destination register
    hw[0] bit 8       condition-code WRITE enable (cc_update)
    hw[0] bits 9..12  write mask
    hw[0] bits 22..23 precision (0 fp32, 1 fp16, 2 fx12)
    hw[0] bits 24..29 opcode
    hw[0] bit 30      destination is NONE (writes the condition register
                      only - the shape a kill's guard and a KIL both use)
    hw[1] bits 18..20 condition-code TEST
    hw[N] bits 0..1   source register type (0 temp, 1 input, 2 const)

Output, one blank-separated line per instruction:

    <index> <opcode-hex> prec=N ccw=0|1 cc=<NE|EQ|TR|...> none=0|1
    dst=N mask=0xN in=<count> const=<count> end=0|1

Inline constant blocks are 16 bytes of DATA following the instruction that
names them; this walk advances past them, so a literal is never decoded as
an opcode.
"""

import re
import sys

TEMP, INPUT, CONST = 0, 1, 2

# NVFX_COND_*, the condition-code test a KIL or a predicated write uses.
COND = {
    0: "FL", 1: "LT", 2: "EQ", 3: "LE",
    4: "GT", 5: "NE", 6: "GE", 7: "TR",
}


def unswap(v):
    """Words are printed as the hardware stores them: halfwords swapped."""
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def groups(path):
    out = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
            if not m:
                continue
            words = [unswap(int(x, 16)) for x in m.group(2).split()]
            if len(words) == 4:
                out.append(words)
    return out


def decode(path):
    gs = groups(path)
    i = 0
    while i < len(gs):
        w = gs[i]
        consts = sum(1 for s in (1, 2, 3) if (w[s] & 3) == CONST)
        inputs = sum(1 for s in (1, 2, 3) if (w[s] & 3) == INPUT)
        yield {
            "index": i,
            "opcode": (w[0] >> 24) & 0x3F,
            "prec": (w[0] >> 22) & 3,
            "ccw": (w[0] >> 8) & 1,
            "cond": (w[1] >> 18) & 7,
            "none": (w[0] >> 30) & 1,
            "dst": (w[0] >> 1) & 0x3F,
            "mask": (w[0] >> 9) & 0xF,
            "inputs": inputs,
            "consts": consts,
            "end": w[0] & 1,
        }
        i += 1 + (1 if consts else 0)


def main():
    for path in sys.argv[1:]:
        for d in decode(path):
            print("%d 0x%02X prec=%d ccw=%d cc=%s none=%d dst=%d mask=0x%X "
                  "in=%d const=%d end=%d"
                  % (d["index"], d["opcode"], d["prec"], d["ccw"],
                     COND.get(d["cond"], "?"), d["none"], d["dst"],
                     d["mask"], d["inputs"], d["consts"], d["end"]))


if __name__ == "__main__":
    main()
