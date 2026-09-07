"""Name the SOURCES of an NV40 fragment instruction (t_c83277c9).

`ucode_decode.py` decodes hw[0] and COUNTS input and const sources; it never
names one.  That is why every test that needed a source register or a swizzle
grew its own decoder - six of them by 2026-09-06, two of which were wrong on
the evening they were written.  This module is the shared answer, and it is a
separate file from ucode_decode.py so that module's ten callers are untouched.

TWO THINGS MAKE NAMING A SOURCE HARDER THAN READING A FIELD, and both are
measured rather than assumed.

1.  AN UNUSED SLOT IS INDISTINGUISHABLE FROM A READ OF R0.  Every instruction
    carries three source words whatever its arity, and the unused ones encode
    as TEMP register 0 with the identity swizzle.  Measured on a three-
    instruction shader:

        MOV  dst=R2  s0=INPUT  s1=TEMP R0  s2=TEMP R0
        MAD  dst=R1  s0=TEMP R1  s1=CONST R0  s2=TEMP R2

    Nothing in the words says which of MOV's three slots the hardware reads.
    ucode_decode.py's header records what happens to a check that ignores
    this: an invariant of the form "every temp source must have been written"
    fired on 209 of 209 shaders, including known-good ones.  So a source
    printer needs an OPCODE-TO-ARITY table, and that table is a second source
    of truth about the ISA - it is `ARITY` below, and `verify_arity` is how it
    earns its place.

2.  AN INPUT SOURCE'S REGISTER IS NOT IN THE SOURCE WORD.  It is hw[0] bits
    13..16, ONE selector for the whole instruction; the slot's own register
    field stays zero.  Measured on four shaders differing only in the varying
    they read:

        TEXCOORD0 -> input_src=4 (TC0)   slot register field = 0
        TEXCOORD1 -> input_src=5 (TC1)   slot register field = 0
        TEXCOORD3 -> input_src=7 (TC3)   slot register field = 0
        COLOR0    -> input_src=1 (COL0)  slot register field = 0

    A printer that reported the slot field would report "R0" for every
    varying in the corpus, and one that reported two input sources as two
    different registers would be inventing a distinction the hardware cannot
    express - which is the other end of the one-input-per-instruction rule
    ucode_operand_rules.py already checks (t_40dd8159).

Nothing here guesses.  An opcode with no ARITY entry reports its sources as
UNKNOWN and names none of them, rather than printing three plausible reads.
"""

import struct
import sys

TEMP, INPUT, CONST = 0, 1, 2
TYPE_NAME = {TEMP: "temp", INPUT: "input", CONST: "const", 3: "type3"}

# hw[0] bits 13..16.  NVFX_FP_OP_INPUT_SRC_*; TC(n) is 0x4 + n.
INPUT_NAME = {
    0x0: "WPOS", 0x1: "COL0", 0x2: "COL1", 0x3: "FOGC",
    0x4: "TEX0", 0x5: "TEX1", 0x6: "TEX2", 0x7: "TEX3",
    0x8: "TEX4", 0x9: "TEX5", 0xA: "TEX6", 0xB: "TEX7",
    0xE: "FACING",
}

OPCODE_NAME = {
    0x00: "NOP", 0x01: "MOV", 0x02: "MUL", 0x03: "ADD", 0x04: "MAD",
    0x05: "DP3", 0x06: "DP4", 0x07: "DST", 0x08: "MIN", 0x09: "MAX",
    0x0A: "SLT", 0x0B: "SGE", 0x0C: "SLE", 0x0D: "SGT", 0x0E: "SNE",
    0x0F: "SEQ", 0x10: "FRC", 0x11: "FLR", 0x12: "KIL", 0x13: "PK4B",
    0x14: "UP4B", 0x15: "DDX", 0x16: "DDY", 0x17: "TEX", 0x18: "TXP",
    0x19: "TXD", 0x1A: "RCP", 0x1B: "RSQ", 0x1C: "EX2", 0x1D: "LG2",
    0x20: "STR", 0x21: "SFL", 0x22: "COS", 0x23: "SIN", 0x24: "PK2H",
    0x25: "UP2H", 0x27: "PK4UB", 0x28: "UP4UB", 0x29: "PK2US",
    0x2A: "UP2US", 0x2E: "DP2A", 0x31: "TXB", 0x38: "DP2", 0x39: "NRM",
    0x3A: "DIV", 0x3B: "DIVSQR", 0x3E: "FENCBR",
}

# How many of the three source slots an opcode actually READS.  Absent means
# unknown: sources are not named for it.  verify_arity() below is what keeps
# this table honest against real emissions - it can catch an entry that is too
# LOW directly, and one that is too HIGH only by the absence of evidence, so
# the two directions are reported separately.
ARITY = {
    0x00: 0,                                              # NOP
    0x12: 0, 0x3E: 0,                                     # KIL FENCBR - see below
    0x01: 1, 0x10: 1, 0x11: 1,                            # MOV FRC FLR
    0x1A: 1, 0x1B: 1, 0x1C: 1, 0x1D: 1,                   # RCP RSQ EX2 LG2
    0x22: 1, 0x23: 1, 0x39: 1,                            # COS SIN NRM
    0x13: 1, 0x14: 1, 0x24: 1, 0x25: 1,                   # PK4B UP4B PK2H UP2H
    0x27: 1, 0x28: 1, 0x29: 1, 0x2A: 1,                   # PK4UB UP4UB PK2US UP2US
    0x15: 1, 0x16: 1,                                     # DDX DDY
    0x17: 1, 0x18: 1, 0x31: 1,                            # TEX TXP TXB
    0x02: 2, 0x03: 2, 0x05: 2, 0x06: 2, 0x07: 2,          # MUL ADD DP3 DP4 DST
    0x08: 2, 0x09: 2, 0x38: 2, 0x3A: 2, 0x3B: 2,          # MIN MAX DP2 DIV DIVSQR
    0x0A: 2, 0x0B: 2, 0x0C: 2, 0x0D: 2, 0x0E: 2, 0x0F: 2, # SLT SGE SLE SGT SNE SEQ
    0x20: 2, 0x21: 2,                                     # STR SFL
    0x04: 3, 0x19: 3, 0x2E: 3,                            # MAD TXD DP2A
}
# KIL reads NO source operand: it kills on the CONDITION CODE, whose test is
# hw[1] bits 18..20 and whose value a previous instruction wrote with
# cc_update.  This entry started at 1 and the verifier below caught it on its
# first run - 23 KILs across 180 in-repo fragment containers and not one of
# them ever carried anything but the padding signature in slot 0.  That is
# what the table is for.
#
# FENCBR (0x3E) is the RSX's "fence before read" extension - it is not in
# nvfx_shader.h's opcode list, which is why a decoder that reads only that
# header reports the 189 of them in the in-repo fragment corpus as an unknown
# opcode.  FpAssembler::emitFencbr names it and writes all three source slots
# as defaults, so its arity is 0.  The name comes from OUR ENCODER, which is
# the available source of truth here.

IDENTITY_SWIZZLE = 0xE4      # .xyzw

HEADER_BYTES = 32            # eight u32 before the parameter table
SUBTYPE_BYTES = 22           # cg_container_fp.cpp's programSubtypeBytes
PARAM_RECORD_BYTES = 48      # twelve u32 per parameter, same file
OUT_NONE_BIT = 30            # NV40_FP_OP_OUT_NONE: this instruction writes nothing
FP_PROFILE = 0x00001B5C      # kProfileFpRsx
FORMAT_REVISION = 6          # kBinaryFormatRevision

# The ABS modifier is NOT at one position: it is a per-slot bit in a DIFFERENT
# word for each source (NVFX_FP_OP_SRC0/1/2_ABS).  A reader that assumed one
# position would report slot 0's abs as slot 1's and miss it entirely on the
# slot the assembler writes at bit 29.
ABS_BIT = {1: (1, 29), 2: (2, 18), 3: (3, 18)}   # slot word -> (word, bit)


class ContainerError(Exception):
    """The bytes are not a container this module can walk.

    Raised, never printed as a traceback: a malformed input is a refusal with
    a reason, and a reader that dies with a stack trace cannot be told apart
    from a reader with a bug.
    """


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def ucode_words(blob):
    """The instruction words of an FP container, in LOGICAL (unswapped) form.

    The subtype block is located from the header's programOffset - word 5 -
    and NOT derived from the ucode offset: the writer stores the locator
    explicitly, and deriving it from alignment reads padding the moment the
    layout changes (measured the hard way on t_1722b8bc).
    """
    if len(blob) < 32:
        raise ContainerError("shorter than a container header (%d bytes)" % len(blob))
    (profile, rev, total, nparams, header_size,
     program_off, ucode_size, ucode_off) = struct.unpack_from(">8I", blob, 0)
    if profile != FP_PROFILE:
        raise ContainerError("profile 0x%08x is not sce_fp_rsx (0x%08x)"
                             % (profile, FP_PROFILE))
    if rev != FORMAT_REVISION:
        raise ContainerError("container format revision %d, this reader knows %d"
                             % (rev, FORMAT_REVISION))
    if total != len(blob):
        raise ContainerError("header says %d bytes, file is %d" % (total, len(blob)))
    # A POINTER THAT IS MERELY IN RANGE IS NOT A VALID LOCATOR.  Checking only
    # `program_off < len(blob)` accepts programOffset = 0 (the subtype block
    # overlapping the header) and programOffset = len - 1 (a 22-byte block
    # running off the end), and the ucode still decodes because it is found by
    # its own offset - so the container reads clean while its metadata points
    # at nothing.  Every extent is checked against every other.
    # Word 4 is where the parameter table BEGINS, not where it ends - reading
    # it as an end lets programOffset land in the MIDDLE of the table and the
    # container still decodes, because the ucode is found by its own offset
    # (codex).  The table's extent has to be computed, and computing it also
    # bounds the parameter count: an nparams of 0xffffffff runs the end past
    # any real file.
    if header_size < HEADER_BYTES:
        raise ContainerError("parameterArray starts at %d, inside the %d-byte header"
                             % (header_size, HEADER_BYTES))
    table_end = header_size + nparams * PARAM_RECORD_BYTES
    if table_end > len(blob):
        raise ContainerError(
            "%d parameters starting at %d need %d bytes, the file is %d"
            % (nparams, header_size, table_end - header_size, len(blob)))
    if program_off < table_end:
        raise ContainerError(
            "programOffset %d is inside the parameter table, which spans [%d, %d)"
            % (program_off, header_size, table_end))
    if program_off + SUBTYPE_BYTES > len(blob):
        raise ContainerError(
            "the program subtype block runs past the end: programOffset %d + %d "
            "> %d" % (program_off, SUBTYPE_BYTES, len(blob)))
    if ucode_size % 4:
        raise ContainerError("ucodeSize %d is not a whole number of words" % ucode_size)
    if ucode_off < program_off + SUBTYPE_BYTES:
        raise ContainerError(
            "ucodeOffset %d starts inside the program subtype block "
            "(programOffset %d + %d)" % (ucode_off, program_off, SUBTYPE_BYTES))
    if ucode_off + ucode_size > len(blob):
        raise ContainerError("ucode runs past the end: offset %d + size %d > %d"
                             % (ucode_off, ucode_size, len(blob)))
    raw = blob[ucode_off:ucode_off + ucode_size]
    return [unswap(struct.unpack_from(">I", raw, i)[0]) for i in range(0, len(raw), 4)]


def instructions(words):
    """Walk instruction groups, stepping over inline constant blocks.

    A block is four words of DATA following the instruction that names it; a
    walk that does not step over it decodes a float as an opcode.
    """
    i = 0
    while i + 4 <= len(words):
        w = words[i:i + 4]
        has_const = any((w[s] & 3) == CONST for s in (1, 2, 3))
        i += 4
        const = None
        if has_const:
            if i + 4 > len(words):
                raise ContainerError(
                    "instruction %d names an inline constant but the block is "
                    "truncated" % (len(words) // 4))
            const = words[i:i + 4]
            i += 4
        yield w, const
    if i != len(words):
        raise ContainerError("ucode ends mid-instruction: %d trailing word(s)"
                             % (len(words) - i))


def source(w, slot):
    """Decode one source slot of an instruction into a dict."""
    s = w[slot]
    kind = s & 3
    abs_word, abs_bit = ABS_BIT[slot]
    out = {
        "slot": slot - 1,
        "type": kind,
        "swizzle": (s >> 9) & 0xFF,
        "negate": (s >> 17) & 1,
        # A source read at HALF precision names an H register, not the R
        # register of the same number: H0 and H1 are the two halves of R0.
        # Dropping this bit misnames the register the instruction reads.
        "half": (s >> 8) & 1,
        # |x| changes the value, so a render that omits it is wrong about
        # what the instruction computes, not merely terse.
        "abs": (w[abs_word] >> abs_bit) & 1,
    }
    if kind == INPUT:
        # hw[0] bits 13..16, one selector for the whole instruction.
        sel = (w[0] >> 13) & 0xF
        out["reg"] = None
        out["name"] = INPUT_NAME.get(sel, "input%d" % sel)
    else:
        out["reg"] = (s >> 2) & 0x3F
        if kind == TEMP:
            out["name"] = "%s%d" % ("H" if out["half"] else "R", out["reg"])
        else:
            out["name"] = "c%d" % out["reg"]
    return out


def swizzle_text(swz):
    return "".join("xyzw"[(swz >> (2 * k)) & 3] for k in range(4))


def looks_like_padding(w, slot):
    """The signature of a slot the hardware does not read: TEMP R0, .xyzw."""
    s = w[slot]
    return (s & 3) == TEMP and ((s >> 2) & 0x3F) == 0 and ((s >> 9) & 0xFF) == IDENTITY_SWIZZLE


def render(w, index):
    op = (w[0] >> 24) & 0x3F
    mask = (w[0] >> 9) & 0xF
    dst = (w[0] >> 1) & 0x3F
    mask_text = "".join(c for c, b in zip("xyzw", (1, 2, 4, 8)) if mask & b) or "-"
    if (w[0] >> OUT_NONE_BIT) & 1:
        # OUT_NONE: the instruction writes NO register, and the destination
        # and mask fields are then meaningless - FpAssembler::emitFencbr sets
        # register 0x3F and mask 0xF into them deliberately, to match the
        # reference byte for byte.  Rendering those as a write to R63.xyzw
        # invents a destination exactly as naming an unused slot invents a
        # read, so they are marked raw and the destination is named none.
        dst_text = "dst=none"
        mask_text = "%s(raw)" % mask_text
    else:
        dst_text = "dst=%s%d" % ("H" if (w[0] >> 7) & 1 else "R", dst)
    parts = [
        "%d" % index,
        OPCODE_NAME.get(op, "op%02X" % op),
        dst_text,
        "mask=%s" % mask_text,
        "prec=%d" % ((w[0] >> 22) & 3),
        "sat=%d" % ((w[0] >> 31) & 1),
        "end=%d" % (w[0] & 1),
    ]
    arity = ARITY.get(op)
    if arity is None:
        parts.append("srcs=UNKNOWN")
    else:
        for slot in range(1, arity + 1):
            s = source(w, slot)
            text = "%s.%s" % (s["name"], swizzle_text(s["swizzle"]))
            if s["abs"]:
                text = "|%s|" % text
            if s["negate"]:
                text = "-" + text
            parts.append("s%d=%s" % (s["slot"], text))
    return " ".join(parts)


def verify_arity(paths):
    """Check ARITY against real emissions, reporting both directions.

    TOO LOW is caught directly: a slot this table calls unused carries
    something other than the padding signature.  TOO HIGH cannot be caught
    that way - a genuine read of R0.xyzw is byte-identical to padding - so it
    is reported as absence of evidence: an opcode whose slot NEVER carried
    anything but padding across the whole input.  Absence of evidence is not
    a finding; it is a list to go and measure.
    """
    contradicted, never_used, seen = [], {}, {}
    for path in paths:
        # A file this walk cannot read is a REFUSAL, not a crash, and not a
        # silent skip either: it is reported and it fails the run.
        try:
            blob = open(path, "rb").read()
            walk = list(instructions(ucode_words(blob)))
        except (OSError, ContainerError) as err:
            contradicted.append((path, None, None, str(err)))
            continue
        for w, _const in walk:
            op = (w[0] >> 24) & 0x3F
            arity = ARITY.get(op)
            if arity is None:
                continue
            seen[op] = seen.get(op, 0) + 1
            for slot in (1, 2, 3):
                real = not looks_like_padding(w, slot)
                if slot > arity and real:
                    contradicted.append((path, op, slot - 1, None))
                if slot <= arity:
                    key = (op, slot - 1)
                    never_used[key] = never_used.get(key, True) and not real
    return contradicted, [k for k, v in never_used.items() if v], seen


def main(argv):
    if len(argv) >= 2 and argv[1] == "--verify-arity":
        contradicted, unused, seen = verify_arity(argv[2:])
        for path, op, slot, err in contradicted:
            if err is not None:
                print("UNREADABLE %s: %s" % (path, err))
            else:
                print("CONTRADICTED %s: %s slot %d carries a real source but "
                      "ARITY says it is unused"
                      % (path, OPCODE_NAME.get(op, "op%02X" % op), slot))
        for op, slot in sorted(unused):
            print("UNWITNESSED %s slot %d: declared a source, never once carried "
                  "anything but padding in this input" % (OPCODE_NAME.get(op, "op%02X" % op), slot))
        print("opcodes seen: %s" % ", ".join(
            "%s=%d" % (OPCODE_NAME.get(o, "op%02X" % o), n) for o, n in sorted(seen.items())))
        return 1 if contradicted else 0
    for path in argv[1:]:
        try:
            words = ucode_words(open(path, "rb").read())
            for n, (w, _const) in enumerate(instructions(words)):
                print(render(w, n))
        except (OSError, ContainerError) as err:
            sys.stderr.write("fp_sources: %s: %s\n" % (path, err))
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
