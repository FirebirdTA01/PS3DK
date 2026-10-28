#!/usr/bin/env python3
"""Checks for toc-edit-ilp32-test.sh.

  loads OBJDUMP NM ELF FUNC...  list the addresses TOC-relative loads read
  shape RELOCS                  exit 0 if objdump -r output has an ADDR64 .toc
                                entry at 4 mod 8 read through .toc+entry+4
  exact GOT REF                 exit 0 only if GOT and REF hold the same load
                                lines the same number of times, REF nonempty
  subset GOT REF                exit 0 only if every GOT line is in REF
  selftest                      run exact/subset on doctored inputs

loads: for each function (an .opd descriptor symbol; its first word is the
code address) every 'lwz/ld rX,d(r2)' and 'addis rY,r2,hi' + 'lwz/ld rX,lo(rY)'
up to the first blr is resolved to its TOC slot, the slot is read from the
file, and the value is printed as 'FUNC OP SYMBOL+OFF' (or 'no-symbol:VALUE').
Exits 1 if a function is missing or has no instructions before blr, or if a
read is not wholly inside a file-backed section of the file."""
import collections
import re
import subprocess
import sys


def read_lines(path):
    return collections.Counter(line for line in open(path).read().splitlines() if line)


def exact(got, ref):
    """Problems that make GOT differ from REF, with multiplicities."""
    if not ref:
        return ['the reference has no loads']
    out = ['    missing load: ' + line for line in sorted((ref - got).elements())]
    out += ['    unexpected load: ' + line for line in sorted((got - ref).elements())]
    return out


def subset(got, ref):
    return ['    unexpected load: ' + line for line in sorted((got - ref).elements())]


def selftest():
    ref = collections.Counter(['draw lwz g_flat+0xc', 'sky ld g_flat+0xc', 'passes ld g_passes+0x1c'])
    cases = [
        ('exact, identical', exact(ref.copy(), ref), False),
        ('exact, empty', exact(collections.Counter(), ref), True),
        ('exact, one load missing', exact(ref - collections.Counter(['sky ld g_flat+0xc']), ref), True),
        ('exact, wrong load', exact(ref - collections.Counter(['draw lwz g_flat+0xc'])
                                    + collections.Counter(['draw lwz no-symbol:0x0']), ref), True),
        ('exact, duplicated load', exact(ref + collections.Counter(['sky ld g_flat+0xc']), ref), True),
        ('exact, empty reference', exact(collections.Counter(), collections.Counter()), True),
        ('subset, empty', subset(collections.Counter(), ref), False),
        ('subset, wrong load', subset(collections.Counter(['draw lwz no-symbol:0x0']), ref), True),
    ]
    bad = 0
    for name, problems, should_fail in cases:
        ok = bool(problems) == should_fail
        bad += not ok
        print('toc-edit-ilp32-check selftest: %s %s' % ('ok  ' if ok else 'FAIL', name))
    return 1 if bad else 0


mode = sys.argv[1] if len(sys.argv) > 1 else ''
if mode == 'selftest':
    sys.exit(selftest())
if mode == 'shape':
    t = open(sys.argv[2]).read()
    toc = re.search(r'RELOCATION RECORDS FOR \[\.toc\]:\n.*?\n(.*?)(?:\n\n|\Z)', t, re.S)
    odd = [int(o, 16) for o in re.findall(r'^([0-9a-f]+)\s+R_PPC64_ADDR64', toc.group(1) if toc else '', re.M)
           if int(o, 16) % 8 == 4]
    mid = {int(a, 16) for a in re.findall(r'R_PPC64_TOC16_LO\s+\.toc\+0x([0-9a-f]+)$', t, re.M)}
    sys.exit(0 if any(o + 4 in mid for o in odd) else 1)
if mode in ('exact', 'subset'):
    problems = (exact if mode == 'exact' else subset)(read_lines(sys.argv[2]), read_lines(sys.argv[3]))
    for line in problems:
        print(line)
    sys.exit(1 if problems else 0)
if mode != 'loads' or len(sys.argv) < 6:
    sys.exit('usage: toc-edit-ilp32-check.py loads|shape|exact|subset|selftest ...')

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
if '.TOC.' not in syms:
    sys.exit('toc-edit-ilp32-check: %s has no .TOC. symbol' % elf)
toc = syms['.TOC.']
hdr = subprocess.run([objdump, '-h', elf], capture_output=True, text=True, check=True).stdout
secs = [(int(m.group(3), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5))
        for m in re.finditer(r'^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)[^\n]*\n\s*(.*)$',
                             hdr, re.M)]
data = open(elf, 'rb').read()


def word(addr, n):
    """The n-byte big-endian value at addr; all n bytes must be file-backed."""
    for vma, size, off, flags in secs:
        if 'CONTENTS' in flags and vma <= addr and addr + n <= vma + size:
            start = off + addr - vma
            if start + n > len(data):
                break
            return int.from_bytes(data[start:start + n], 'big')
    sys.exit('toc-edit-ilp32-check: %d bytes at %#x are not inside a file-backed section' % (n, addr))


def name_of(v):
    for s, a in syms.items():
        if s in sizes and a <= v < a + max(sizes[s], 1):
            return '%s+%#x' % (s, v - a)
    return 'no-symbol:%#x' % v


for f in funcs:
    if f not in syms:
        sys.exit('toc-edit-ilp32-check: %s has no symbol %s' % (elf, f))
    code = word(syms[f], 4)
    dis = subprocess.run([objdump, '-d', '--no-show-raw-insn', '--start-address=%#x' % code,
                          '--stop-address=%#x' % (code + 0x400), elf],
                         capture_output=True, text=True, check=True).stdout
    if '\tblr' not in dis:
        sys.exit('toc-edit-ilp32-check: no blr within 1 KiB of %s (%#x)' % (f, code))
    body = dis.split('\tblr', 1)[0]
    # [ \t], not \s: a bare 'nop' must not run on into the next line.
    insns = re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+(\w+)[ \t]*(\S*)', body, re.M)
    if not insns:
        sys.exit('toc-edit-ilp32-check: no instructions parsed for %s (%#x)' % (f, code))
    hi = {}
    for op, args in insns:
        m = re.match(r'(r\d+),(\S+)$', args)
        if not m:
            continue
        rd, rest = m.groups()
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
