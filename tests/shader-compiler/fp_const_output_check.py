"""The colour output's VALUE, computed from the program, not read off a block.

A const-fold row is about what the shader PAINTS.  Asserting the inline
const block and the swizzle that reads it is not that assertion: codex
flipped SRC0's negate bit on the folded MOV of const-division-fold-test's
`plain` container, left the block, the swizzle, the status and every byte of
stdout untouched, and the whole guard - all four self-checks and the COND_FL
control included - stayed green while the shader painted (-0,-1,-3,-2)
instead of (0,1,3,2).  The block says what is STORED; only walking the
program says what is WRITTEN.

So this evaluates R0 lane by lane and compares BIT PATTERNS, because the
rule these rows exist for distinguishes +0 from -0 and a float comparison
does not.

IT REFUSES WHAT IT DOES NOT MODEL, and a refusal is a failure, never a pass:
any opcode but MOV (FENCBR writes nothing and is stepped over), a write to
anything but R0, a half-bank source or destination, saturation, a non-TR
condition test, a condition-code write, an OUT_NONE row with a real opcode,
relative addressing, a source that is not the instruction's own const block,
a program whose last instruction does not carry END, an instruction after
the END, or a lane of R0 left unwritten.  A checker that steps over what it
cannot read reports a green about a program it did not evaluate.

MODIFIERS ARE APPLIED IN THE ORDER THE HARDWARE APPLIES THEM: swizzle, then
abs, then negate - so |-x| is |x| and -|x| is negative.  Getting that order
wrong would make the negate control pass for the wrong reason.

usage: fp_const_output_check.py <container> <w0> <w1> <w2> <w3>
           [--control negate|swizzle|mask|scale|precision|no-end]

The four words are the expected R0 lanes as 8-digit hex, in the order the
program paints them.

--control mutates the CONTAINER BYTES and requires this checker to REJECT
the result: `negate` sets SRC0's negate bit on the first R0 write (codex's
mutation), `swizzle` rewrites its swizzle to .xxxx, `mask` drops the write
mask to .x, `scale` sets a 2X destination scale, `precision` sets a
non-fp32 precision, and `no-end` clears END so the program never finishes.  Exit 0 means the mutant was rejected; exit 1 means this checker
is too weak to see it.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
import fp_sources  # noqa: E402
# THE ENVELOPE IS SHARED, NOT RE-DERIVED.  inline_factor_check already
# refuses the fields an operand-level evaluator cannot see, and this file
# missed two of them by writing its own list: destination SCALE (hw[2] bits
# 28..30 - codex set 2X on a clean container and every row here stayed
# green) and non-fp32 PRECISION (hw[0] bits 22..23).  Importing its table
# means a field added there is added here, instead of the two drifting
# until someone mutates the difference.
import inline_factor_check  # noqa: E402

MOV = 0x01
FENCBR = 0x3E
COND_TR = 7


class Unmodelled(Exception):
    """The program contains something this evaluator does not read."""


def _f2b(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def _b2f(w):
    return struct.unpack(">f", struct.pack(">I", w))[0]


def evaluate(blob):
    """The four words R0 holds when the program ends."""
    words = fp_sources.ucode_words(blob)
    lanes = [None, None, None, None]
    ended = False
    for n, (w, block) in enumerate(fp_sources.instructions(words)):
        if ended:
            raise Unmodelled("instruction %d follows the END row" % n)
        op = (w[0] >> 24) & 0x3F
        last = (w[0] >> 0) & 1
        if op == FENCBR:
            if last:
                raise Unmodelled("FENCBR at instruction %d carries END" % n)
            continue
        if op != MOV:
            raise Unmodelled("opcode 0x%02X at instruction %d (MOV only)" % (op, n))
        for shift, mask, what in inline_factor_check.FP_MODES:
            if (w[0] >> shift) & mask:
                raise Unmodelled("%s at instruction %d" % (what, n))
        if (w[2] >> 28) & 7:
            raise Unmodelled("destination scale at instruction %d" % n)
        if ((w[1] >> 18) & 7) != COND_TR:
            raise Unmodelled("a condition test at instruction %d" % n)
        dst = (w[0] >> 1) & 0x3F
        if dst != 0:
            raise Unmodelled("a write to R%d at instruction %d (R0 only)" % (dst, n))
        if block is None:
            raise Unmodelled("instruction %d has no inline const block" % n)
        src = fp_sources.source(w, 1)
        if src["type"] != fp_sources.CONST:
            raise Unmodelled("source 0 of instruction %d is not the const block" % n)
        if src["half"]:
            raise Unmodelled("a half-bank source at instruction %d" % n)
        if (w[3] >> 1) & 1:
            raise Unmodelled("relative const addressing at instruction %d" % n)
        mask = (w[0] >> 9) & 0xF
        for lane in range(4):
            if not (mask >> lane) & 1:
                continue
            value = block[(src["swizzle"] >> (2 * lane)) & 3]
            if src["abs"]:
                value &= 0x7FFFFFFF
            if src["negate"]:
                value ^= 0x80000000
            lanes[lane] = value
        ended = bool(last)
    if not ended:
        raise Unmodelled("the final instruction does not carry END")
    for lane in range(4):
        if lanes[lane] is None:
            raise Unmodelled("lane %s of R0 is never written" % "xyzw"[lane])
    return lanes


def problems(blob, want):
    try:
        got = evaluate(blob)
    except (Unmodelled, fp_sources.ContainerError) as exc:
        return "unmodelled: %s" % exc
    if got != want:
        return ("the colour output is [%s] (%s); the reference paints [%s] (%s)"
                % (" ".join("%08x" % v for v in got),
                   ", ".join(repr(_b2f(v)) for v in got),
                   " ".join("%08x" % v for v in want),
                   ", ".join(repr(_b2f(v)) for v in want)))
    return None


# --- container mutations, so the row's own artifact can prove the check ----

def _first_r0_write(blob):
    words = fp_sources.ucode_words(blob)
    off = struct.unpack_from(">8I", blob, 0)[7]
    i = 0
    for w, block in fp_sources.instructions(words):
        op = (w[0] >> 24) & 0x3F
        if op == MOV and ((w[0] >> 1) & 0x3F) == 0:
            return off, i, w
        i += 4 + (4 if block is not None else 0)
    raise SystemExit("FAIL: no R0-writing MOV to mutate")


def mutate(blob, kind):
    off, i, w = _first_r0_write(blob)
    out = bytearray(blob)
    if kind == "negate":                      # codex's mutation
        new = w[1] | (1 << 17)
        struct.pack_into(">I", out, off + (i + 1) * 4, fp_sources.unswap(new))
    elif kind == "swizzle":
        new = (w[1] & ~(0xFF << 9)) | (0x00 << 9)      # .xxxx
        struct.pack_into(">I", out, off + (i + 1) * 4, fp_sources.unswap(new))
    elif kind == "mask":
        new = (w[0] & ~(0xF << 9)) | (0x1 << 9)        # .x only
        struct.pack_into(">I", out, off + i * 4, fp_sources.unswap(new))
    elif kind == "scale":                     # codex's second mutation
        new = (w[2] & ~(7 << 28)) | (1 << 28)          # 2X destination scale
        struct.pack_into(">I", out, off + (i + 2) * 4, fp_sources.unswap(new))
    elif kind == "precision":
        new = (w[0] & ~(3 << 22)) | (1 << 22)          # a non-fp32 precision
        struct.pack_into(">I", out, off + i * 4, fp_sources.unswap(new))
    elif kind == "no-end":
        # Clear END on whichever instruction carries it.  A program that
        # never ends is not a program whose output can be read.
        words = fp_sources.ucode_words(blob)
        j = 0
        cleared = False
        for ww, block in fp_sources.instructions(words):
            if ww[0] & 1:
                struct.pack_into(">I", out, off + j * 4,
                                 fp_sources.unswap(ww[0] & ~1))
                cleared = True
            j += 4 + (4 if block is not None else 0)
        if not cleared:
            raise SystemExit("FAIL: no END row to clear")
    else:
        raise SystemExit("FAIL: unknown control %r" % kind)
    return bytes(out)


def main(argv):
    if len(argv) < 6:
        raise SystemExit(__doc__)
    path = argv[1]
    want = [int(x, 16) for x in argv[2:6]]
    blob = open(path, "rb").read()
    if len(argv) == 8 and argv[6] == "--control":
        kind = argv[7]
        clean = problems(blob, want)
        if clean:
            sys.stderr.write("FAIL: the container is not clean before the "
                             "control: %s\n" % clean)
            return 1
        mutant = mutate(blob, kind)
        if mutant == blob:
            sys.stderr.write("FAIL: the %s control changed nothing\n" % kind)
            return 1
        open(path + "." + kind, "wb").write(mutant)
        why = problems(mutant, want)
        if not why:
            sys.stderr.write("FAIL: the %s control was ACCEPTED - this check "
                             "cannot see it\n" % kind)
            return 1
        print("control %s rejected: %s" % (kind, why))
        return 0
    why = problems(blob, want)
    if why:
        sys.stderr.write("FAIL: %s\n" % why)
        return 1
    print("colour output = [%s]" % " ".join("%08x" % v for v in want))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
