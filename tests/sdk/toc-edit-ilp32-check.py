#!/usr/bin/env python3
"""Checks for toc-edit-ilp32-test.sh.

  loads OBJDUMP NM ELF FUNC...  list the addresses TOC-relative loads read
  shape RELOCS                  exit 0 if objdump -r output has an ADDR64 .toc
                                entry at 4 mod 8 read through .toc+entry+4
  compare GOT REF               exit 1 if GOT has a load line REF lacks

loads: for each function (an .opd descriptor symbol; its first word is the
code address) every 'lwz/ld rX,d(r2)' and 'addis rY,r2,hi' + 'lwz/ld rX,lo(rY)'
is resolved to its TOC slot, the slot is read from the file, and the value is
printed as 'FUNC OP SYMBOL+OFF' (or 'no-symbol:VALUE').  Exits 1 if a slot
is outside the file."""
import collections
import re
import subprocess
import sys

if sys.argv[1] == 'shape':
    t = open(sys.argv[2]).read()
    toc = re.search(r'RELOCATION RECORDS FOR \[\.toc\]:\n.*?\n(.*?)(?:\n\n|\Z)', t, re.S)
    odd = [int(o, 16) for o in re.findall(r'^([0-9a-f]+)\s+R_PPC64_ADDR64', toc.group(1) if toc else '', re.M)
           if int(o, 16) % 8 == 4]
    mid = {int(a, 16) for a in re.findall(r'R_PPC64_TOC16_LO\s+\.toc\+0x([0-9a-f]+)$', t, re.M)}
    sys.exit(0 if any(o + 4 in mid for o in odd) else 1)
if sys.argv[1] == 'compare':
    got = collections.Counter(line for line in open(sys.argv[2]).read().splitlines() if line)
    ref = collections.Counter(line for line in open(sys.argv[3]).read().splitlines() if line)
    extra = got - ref
    for line in sorted(extra.elements()):
        print('    unexpected load:', line)
    sys.exit(1 if extra else 0)
assert sys.argv[1] == 'loads'
objdump, nm, elf = sys.argv[2:5]
funcs = sys.argv[5:]
syms = {}
sizes = {}
for line in subprocess.run([nm, '-S', elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4:
        syms[p[3]] = int(p[0], 16)
        sizes[p[3]] = int(p[1], 16)
    elif len(p) == 3:
        syms[p[2]] = int(p[0], 16)
toc = syms['.TOC.']
hdr = subprocess.run([objdump, '-h', elf], capture_output=True, text=True, check=True).stdout
secs = [(int(m.group(3), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5))
        for m in re.finditer(r'^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)[^\n]*\n\s*(.*)$',
                             hdr, re.M)]
data = open(elf, 'rb').read()


def word(addr, n):
    for vma, size, off, flags in secs:
        if vma <= addr < vma + size and 'CONTENTS' in flags:
            return int.from_bytes(data[off + addr - vma:off + addr - vma + n], 'big')
    sys.exit('toc-edit-ilp32-check: address %#x is not file-backed' % addr)


def name_of(v):
    for s, a in syms.items():
        if s in sizes and a <= v < a + max(sizes[s], 1):
            return '%s+%#x' % (s, v - a)
    return 'no-symbol:%#x' % v


for f in funcs:
    code = word(syms[f], 4)
    dis = subprocess.run([objdump, '-d', '--no-show-raw-insn', '--start-address=%#x' % code,
                          '--stop-address=%#x' % (code + 0x400), elf],
                         capture_output=True, text=True, check=True).stdout
    dis = dis.split('\tblr', 1)[0]
    hi = {}
    for m in re.finditer(r'^\s*[0-9a-f]+:\s+(\w+)\s+(r\d+),(\S+)', dis, re.M):
        op, rd, rest = m.groups()
        if op == 'addis' and rest.startswith('r2,'):
            hi[rd] = int(rest.split(',')[1], 0) << 16
            continue
        mm = re.match(r'(-?\d+)\((r\d+)\)$', rest)
        if op in ('lwz', 'ld') and mm:
            lo, base = int(mm.group(1)), mm.group(2)
            if base == 'r2':
                ea = toc + lo
            elif base in hi:
                ea = toc + hi.pop(base) + lo
            else:
                continue
            print(f, op, name_of(word(ea, 4 if op == 'lwz' else 8)))
