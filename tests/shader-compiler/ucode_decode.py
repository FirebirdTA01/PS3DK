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


class CorruptDump(Exception):
    """The log is not a faithful copy of what the compiler printed."""


# A DUMP ROW THAT DOES NOT PARSE USED TO BE SKIPPED SILENTLY, and that is
# how a harness problem became a compiler finding.  Capturing the compiler
# with a merged `2>&1` lets a stderr line interleave INSIDE a hex row, so
# the row stops matching, this function drops it, every later row shifts by
# one, and a constant gets decoded as an instruction writing a register
# nothing reads - a "dead write" that the compiler never emitted.  That cost
# two people an hour of arguing about four accumulation_mad rows where one
# ran the suite under WSL with merged capture and the other natively with
# separate capture, on the SAME binary (codex, 2026-09-07).
#
# The rows are NUMBERED, so the corruption is detectable rather than
# guessable: indices run 0, 1, 2, ... within one program and restart at 0
# when a log holds several.  A gap means a row was dropped, and a line that
# opens like a row but does not parse as four hex words IS the dropped row.
# Both now raise instead of returning a plausible-looking short list.
# AND THE DUMP CARRIES ITS OWN CLOSING CHECK.  One line above the rows the
# compiler prints "NV40 ucode words: N", so the row count is knowable
# independently of the row numbers - and a splice that damages the FINAL row
# without leaving a "<digits>:" line behind is invisible to both rules above.
# Fable measured that shape: a stderr fragment landing between the index
# digits and the colon ("4 src0 kind=1 ..." then ": 8280...") leaves two
# lines, neither of which opens like a row, and no successor index to
# disagree with - four of five rows, silently.  Requiring 4 * rows == N per
# program closes every final-row shape at once, including a tail lost to a
# truncated pipe.
WORDS = re.compile(r"NV40 ucode words:\s*(\d+)")


def groups(path):
    out = []
    expected = 0
    declared = None          # words the dump says this program has
    rows_here = 0            # rows decoded since the last restart

    def close(lineno):
        if declared is None:
            return
        if 4 * rows_here != declared:
            raise CorruptDump(
                "%s:%d: the dump declares %d ucode words - %d rows - and %d "
                "rows were decoded.  The log is not a faithful copy: a row "
                "was lost without leaving a parseable trace, which is what a "
                "stderr line spliced across a row boundary does, and what a "
                "truncated pipe does to the tail.  Capture stdout and stderr "
                "separately and join them after the process exits."
                % (path, lineno, declared, declared // 4, rows_here))

    with open(path, "r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            w = WORDS.search(line)
            if w:
                # a new program's header closes the one before it
                close(lineno)
                declared = int(w.group(1))
                rows_here = 0
                continue
            m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
            if not m:
                # ANY line that opens "<digits>:" is a dump row.  An
                # earlier version of this also required the first payload
                # character to be hex, which let a splice land BEFORE the
                # first word - "173: src0 kind=1 ..." - slip through as
                # prose; on the FINAL row there is no following index to
                # expose the gap either, so the decoder silently returned
                # 173 of 174 rows (codex, review of 1a588e5f).  Measured
                # before widening it: in a real dump, with RSX_DUMP_ORDER
                # both on and off, EVERY line matching "<digits>:" is a hex
                # row - the log's ordinary prose never opens that way.
                if re.match(r"\s*\d+:", line):
                    raise CorruptDump(
                        "%s:%d: a ucode row did not parse as four hex words - "
                        "the log is not a faithful copy of the dump.  The usual "
                        "cause is capturing the compiler with a merged 2>&1, "
                        "which lets a stderr line land inside a row.  Capture "
                        "stdout and stderr separately and join them after the "
                        "process exits.  Row was: %r" % (path, lineno, line[:120]))
                continue
            words = [unswap(int(x, 16)) for x in m.group(2).split()]
            if len(words) != 4:
                raise CorruptDump(
                    "%s:%d: a ucode row carried %d words, not four (%r)"
                    % (path, lineno, len(words), line[:120]))
            index = int(m.group(1))
            if index == 0 and expected != 0:
                # A second program in the same log.  Its header, if it
                # printed one, has already closed the program before it and
                # reset the count; only close here when it did not.
                if rows_here:
                    close(lineno)
                    declared = None
                    rows_here = 0
                expected = 0
            if index != expected:
                raise CorruptDump(
                    "%s:%d: ucode row %d arrived where row %d was expected, so "
                    "at least one row was dropped and every row after it is "
                    "misaligned.  A dropped row decodes later words as opcodes "
                    "and invents register writes that the compiler never "
                    "emitted.  See the merged-capture note above."
                    % (path, lineno, index, expected))
            expected = index + 1
            rows_here += 1
            out.append(words)
    close(lineno if out else 0)
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
