#!/usr/bin/env python3
"""I3a descriptor check on a LINKED SPU ELF (design I3 r2 section 3).

Parses the ELF itself (ELF32, big-endian, EM_SPU=23): the ps3dkprof_spudesc section must be 16 x n bytes, 16-aligned,
equal to [__start_ps3dkprof_spudesc, __stop_ps3dkprof_spudesc); each record i must hold its own LS address
(sh_addr + 16 i), and its name/file pointers must resolve through a PT_LOAD to NUL-terminated file bytes; the
(name, file basename, line) multiset must equal the expected one, which is read from the fixture sources (every
PS3DK_PROF_SPU_ZONE_BEGIN/MARK(w, "name") use and its line).  No relocation section may target the descriptor
section.

usage: i3_desc_check.py ELF --expect N --sources SRC... [--exclude NAME...]
exit 0 = as expected; 1 = mismatch; 2 = unreadable/unsupported ELF"""
import argparse
import os
import re
import struct
import sys

USE = re.compile(r'PS3DK_PROF_SPU_(?:ZONE_BEGIN|MARK)\(\s*[^,()]+\s*,\s*"([^"]+)"\s*\)')


def die(msg, rc=2):
    print(f'DESC FAIL: {msg}')
    sys.exit(rc)


def parse(path):
    try:
        b = open(path, 'rb').read()
    except OSError as x:
        die(f'cannot read {path}: {x}')
    if b[:4] != b'\x7fELF' or b[4] != 1 or b[5] != 2:
        die('not ELF32 big-endian')
    e_type, e_machine = struct.unpack_from('>HH', b, 16)
    if e_machine != 23:
        die(f'e_machine {e_machine}, not EM_SPU')
    e_phoff, e_shoff = struct.unpack_from('>II', b, 28)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('>HHHHH', b, 42)
    if e_shentsize != 40 or e_phentsize != 32 or e_shoff + e_shnum * 40 > len(b) or e_shstrndx >= e_shnum:
        die('section/program header table out of range')
    sh = [struct.unpack_from('>IIIIIIIIII', b, e_shoff + i * 40) for i in range(e_shnum)]
    ph = [struct.unpack_from('>IIIIIIII', b, e_phoff + i * 32) for i in range(e_phnum)]
    stro = sh[e_shstrndx][4]

    def cstr(off, end):
        z = b.find(b'\0', off, end)
        if z < 0:
            return None
        return b[off:z].decode('ascii', 'replace')

    names = [cstr(stro + s[0], len(b)) for s in sh]
    return b, sh, ph, names, cstr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--expect', type=int, required=True)
    ap.add_argument('--sources', nargs='+', required=True)
    ap.add_argument('--exclude', nargs='*', default=[])
    a = ap.parse_args()
    b, sh, ph, names, cstr = parse(a.elf)
    want = []
    for src in a.sources:
        for ln, line in enumerate(open(src, encoding='utf-8'), 1):
            for m in USE.finditer(line):
                if m.group(1) not in a.exclude:
                    want.append((m.group(1), os.path.basename(src), ln))
    if len(want) != a.expect:
        die(f'the sources give {len(want)} expected records, --expect {a.expect}', 1)
    try:
        k = names.index('ps3dkprof_spudesc')
    except ValueError:
        if a.expect == 0:
            print('DESC OK: no descriptor section, as expected')
            return 0
        die('no ps3dkprof_spudesc section', 1)
    _, typ, flags, addr, off, size, *_ = sh[k]
    bad = []
    if size % 16 or addr % 16 or size // 16 != a.expect:
        bad.append(f'section addr 0x{addr:x} size {size} (want {16 * a.expect}, 16-aligned)')
    # symbols
    syms = {}
    for i, s in enumerate(sh):
        if s[1] == 2:  # SHT_SYMTAB
            so, ss, link = s[4], s[5], s[6]
            sstr = sh[link][4]
            for j in range(ss // 16):
                st_name, st_value = struct.unpack_from('>II', b, so + j * 16)
                syms[cstr(sstr + st_name, len(b))] = st_value
    if syms.get('__start_ps3dkprof_spudesc') != addr or syms.get('__stop_ps3dkprof_spudesc') != addr + size:
        bad.append(f'start/stop {syms.get("__start_ps3dkprof_spudesc")}/{syms.get("__stop_ps3dkprof_spudesc")} '
                   f'vs section [0x{addr:x}, 0x{addr + size:x})')
    # relocations targeting the section
    for i, s in enumerate(sh):
        if s[1] in (4, 9) and s[7] == k:  # SHT_RELA / SHT_REL with sh_info = the descriptor section
            bad.append(f'relocation section {names[i]} targets the descriptor section')

    def at_va(va):
        for p in ph:
            p_type, p_off, p_vaddr, _, p_filesz = p[:5]
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return p_off + (va - p_vaddr), p_off + p_filesz
        return None

    got, strs = [], {}
    for i in range(size // 16):
        ls_addr, line, pname, pfile = struct.unpack_from('>IIII', b, off + 16 * i)
        if ls_addr != addr + 16 * i:
            bad.append(f'record {i}: self address 0x{ls_addr:x}, want 0x{addr + 16 * i:x}')
        sv = []
        for p in (pname, pfile):
            loc = at_va(p)
            sv.append(cstr(*loc) if loc else None)
        if None in sv:
            bad.append(f'record {i}: a string pointer (0x{pname:x}, 0x{pfile:x}) does not resolve to file bytes')
            continue
        got.append((sv[0], os.path.basename(sv[1]), line))
        strs[pname], strs[pfile] = len(sv[0]) + 1, len(sv[1]) + 1
    if sorted(got) != sorted(want):
        bad.append(f'records {sorted(got)} != expected {sorted(want)}')
    if bad:
        for x in bad:
            print(f'DESC FAIL: {x}')
        return 1
    print(f'DESC OK: {len(got)} records at 0x{addr:x} ({size} bytes), self-addresses, strings, lines and start/stop as '
          f'expected; {len(strs)} distinct strings, {sum(strs.values())} bytes with NULs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
