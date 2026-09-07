#!/usr/bin/env python3
"""vp_words.py - the VERTEX program's instruction words, one line each.

Container in (CgBinary, our --emit-container output or the reference's),
decoded VP instructions out, restricted to the fields this module has been
measured on.  Written for the run-time array-index slice (t_99b29225) after
the room ruled the older VP slot transcription UNTRUSTED (t_c83277c9): every
field below was read back from reference containers whose source shape
fixed the expected value (a POSITION input, a TEXCOORD1 index, c464..c467
blocks, A0/A1 lanes), and nothing beyond those fields is claimed.

Instruction = four big-endian 32-bit words hw[0..3], from the ucode region
named by header words 6 (size) and 7 (offset).  Fields used:

  hw[0]  bits 15..20  vector destination temp index (ARL: the ADDRESS
                      register index, 0 = A0, 1 = A1 - the hardware reads
                      the same field)
         bit  30      vector result goes to an OUTPUT register
         bit  24      ADDR_REG_SELECT_1: relative reads use A1, not A0
         bits 0..1    ADDR_SWZ: which lane of the address register indexes
  hw[1]  bits 22..26  vector opcode
         bits 12..20  CONST_SRC: the c[] register (9 bits; the block BASE
                      for a relative read)
         bits 8..11   INPUT_SRC: the one input register the instruction
                      may name
         bits 0..7    src0 high byte
  hw[2]  bits 23..31  src0 low 9 bits;  bits 6..22 src1;  bits 0..5 src2 high
  hw[3]  bits 21..31  src2 low 11 bits
         bits 13..16  vector writemask (bit 16 = x, 15 = y, 14 = z, 13 = w)
         bits 2..6    output register when hw[0] bit 30 is set
         bit  1       INDEX_CONST: the const source is relative to A<sel>
         bit  0       last instruction

A 17-bit source field: bits 0..1 type (1 temp, 2 input, 3 const), bits 2..7
temp index, bits 8..15 swizzle (two bits per lane, x first at 14..15),
bit 16 negate.  An unused source slot reads as an input with the
instruction's INPUT_SRC and swizzle xyzw - printed verbatim, not hidden.

Output, one instruction per line:
  <n> <OP> dst=<A0|A1|R<k>|o<k>> mask=<lanes> src0=<s> src1=<s> src2=<s>
where a source is [-]IN<k>.<swz>, [-]R<k>.<swz>, [-]C<k>.<swz>, or for a
relative read [-]C[A<sel>.<lane>+<k>].<swz>.  Exit 2 with a reason for a
container that is not a vertex program or whose ucode region is not a
whole number of instructions - never a traceback.
"""
import struct
import sys

VEC_OPS = {
    0: 'NOP', 1: 'MOV', 2: 'MUL', 3: 'ADD', 4: 'MAD', 5: 'DP3', 6: 'DPH',
    7: 'DP4', 8: 'DST', 9: 'MIN', 10: 'MAX', 11: 'SLT', 12: 'SGE', 13: 'ARL',
    14: 'FRC', 15: 'FLR', 16: 'SEQ', 17: 'SFL', 18: 'SGT', 19: 'SLE',
    20: 'SNE', 21: 'STR', 22: 'SSG', 23: 'ARR', 24: 'ARA', 25: 'TXL',
}
PROFILE_VP = 7003
LANES = 'xyzw'


def decode_source(field, const_reg, input_reg, relative, addr_sel, addr_lane):
    kind = field & 3
    index = (field >> 2) & 0x3F
    swz = ''.join(LANES[(field >> (14 - 2 * lane)) & 3] for lane in range(4))
    neg = '-' if (field >> 16) & 1 else ''
    if kind == 1:
        base = 'R%d' % index
    elif kind == 2:
        base = 'IN%d' % input_reg
    elif kind == 3:
        if relative:
            base = 'C[A%d.%s+%d]' % (addr_sel, LANES[addr_lane], const_reg)
        else:
            base = 'C%d' % const_reg
    else:
        base = '?'
    return '%s%s.%s' % (neg, base, swz)


def decode(blob):
    if len(blob) < 32:
        return None, 'container shorter than its header'
    profile, _, _, _, _, _, ucode_size, ucode_off = struct.unpack_from('>8I', blob, 0)
    if profile != PROFILE_VP:
        return None, 'not a vertex program (profile word %d)' % profile
    if ucode_size % 16 or ucode_off + ucode_size > len(blob):
        return None, 'ucode region is not a whole number of instructions'
    lines = []
    for n in range(ucode_size // 16):
        hw = struct.unpack_from('>4I', blob, ucode_off + n * 16)
        vop = (hw[1] >> 22) & 0x1F
        op = VEC_OPS.get(vop, 'VEC%d' % vop)
        const_reg = (hw[1] >> 12) & 0x1FF
        input_reg = (hw[1] >> 8) & 0xF
        relative = (hw[3] >> 1) & 1
        addr_sel = (hw[0] >> 24) & 1
        addr_lane = hw[0] & 3
        vdst = (hw[0] >> 15) & 0x3F
        if op == 'ARL':
            dst = 'A%d' % vdst
        elif (hw[0] >> 30) & 1:
            dst = 'o%d' % ((hw[3] >> 2) & 0x1F)
        else:
            dst = 'R%d' % vdst
        mask = ''.join(LANES[lane] for lane in range(4)
                       if (hw[3] >> (16 - lane)) & 1) or '-'
        s0 = ((hw[1] & 0xFF) << 9) | ((hw[2] >> 23) & 0x1FF)
        s1 = (hw[2] >> 6) & 0x1FFFF
        s2 = ((hw[2] & 0x3F) << 11) | ((hw[3] >> 21) & 0x7FF)
        srcs = [decode_source(s, const_reg, input_reg, relative, addr_sel, addr_lane)
                for s in (s0, s1, s2)]
        lines.append('%d %s dst=%s mask=%s src0=%s src1=%s src2=%s'
                     % (n, op, dst, mask, srcs[0], srcs[1], srcs[2]))
    return lines, None


def main(argv):
    if len(argv) != 2:
        sys.stderr.write('usage: vp_words.py <container.bin>\n')
        return 2
    with open(argv[1], 'rb') as f:
        blob = f.read()
    lines, why = decode(blob)
    if why:
        sys.stderr.write('vp_words: %s: %s\n' % (argv[1], why))
        return 2
    sys.stdout.write('\n'.join(lines) + ('\n' if lines else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
