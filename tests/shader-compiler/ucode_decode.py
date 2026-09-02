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

AN UNUSED OPERAND SLOT ENCODES AS REGISTER TYPE TEMP, INDEX 0.  Measured on
sd_mad_probe: instruction 0 is ADD (0x03) with src0=INPUT, src1=CONST and
src2=TEMP R0, raw 0x3fe1c800 - a third operand an ADD does not have.  Every
instruction with fewer than three real sources carries dummy TEMP-R0
operands, and NOTHING IN THE UCODE DISTINGUISHES THEM FROM A GENUINE READ OF
R0.  Two consequences for anyone writing a check on these words:

  - A byte-only rule of the form "every temp source must have been written"
    or "no two sources may name one slot" is FALSE as stated.  One such
    invariant, proposed and measured before it was written, fired on 209 of
    209 compiled shaders on every binary including known-good ones.  Making
    it sound needs an opcode -> source-arity table, which is a second source
    of truth about the ISA.
  - A scan that counts these as reads is still sound PROVIDED it excludes
    R0, because the dummies only ever name R0: the over-count can make a
    register look more live, never less, and cannot mask anything about a
    non-zero slot.  That is why colour-reaches-r0-test.sh is correct.

Virtual-register IDENTITY is not in these words at all.  A check that needs
it takes it from the allocator trace (RSX_DUMP_ORDER), whose `kind` field is
VSrcKind: None=0, Temp=1, Input=2, Uniform=3, Literal=4.  Reading Temp as 0
there produces a tool that examines nothing and reports every shader clean.
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
