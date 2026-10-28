#!/usr/bin/env python3
"""Inspect a PPU object or linked PRX ELF for module-export-test.sh.

Reads the ELF directly (big-endian ELF64), so the checks do not depend on
any display tool's column layout.

  module-export-inspect.py object FILE EXPECTED_HEX START_FN STOP_FN
  module-export-inspect.py linked FILE EXPECTED_HEX START_FN STOP_FN

Both modes require exactly one PROGBITS ".ps3dk.module_identity" section,
with SHF_ALLOC clear and contents equal to EXPECTED_HEX, and require the
global module_start / module_stop symbols to be defined at the same section
and value as START_FN / STOP_FN.  "linked" additionally decodes the
management export record reached through the module-info block's export
range and requires its module_start / module_stop addresses to be the two
functions.  Prints one problem per line; exits 1 on any problem, 2 on an
unreadable file.
"""

import struct
import sys

SHT_PROGBITS, SHT_SYMTAB = 1, 2
SHF_ALLOC = 0x2
SHN_UNDEF = 0
NID_START, NID_STOP = 0xBC9A0086, 0xAB779874
IDENT = ".ps3dk.module_identity"


class Elf:
    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
            raise ValueError("not a big-endian ELF64 file")
        self.data = data
        (shoff,) = struct.unpack_from(">Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x3A)
        (phoff,) = struct.unpack_from(">Q", data, 0x20)
        phentsize, phnum = struct.unpack_from(">HH", data, 0x36)
        if shentsize != 64 or shoff + shnum * 64 > len(data):
            raise ValueError("section header table is out of range")
        self.shdrs = []
        for i in range(shnum):
            f = struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * 64)
            self.shdrs.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3],
                                   offset=f[4], size=f[5], link=f[6], info=f[7],
                                   entsize=f[9]))
        strtab = self.shdrs[shstrndx]
        for s in self.shdrs:
            s["name"] = self.cstr(strtab["offset"] + s["name_off"])
            if s["type"] != 8 and s["offset"] + s["size"] > len(data):
                raise ValueError("section %s runs past the end" % s["name"])
        self.phdrs = []
        if phnum and phentsize == 56:
            for i in range(phnum):
                f = struct.unpack_from(">IIQQQQQQ", data, phoff + i * 56)
                self.phdrs.append(dict(type=f[0], offset=f[2], vaddr=f[3], filesz=f[5]))
        self.syms = {}
        for s in self.shdrs:
            if s["type"] != SHT_SYMTAB:
                continue
            names = self.shdrs[s["link"]]
            for i in range(s["size"] // 24):
                n, info, _other, shndx, value, _size = struct.unpack_from(
                    ">IBBHQQ", data, s["offset"] + i * 24)
                name = self.cstr(names["offset"] + n)
                if name and (info >> 4) in (1, 2):  # GLOBAL or WEAK
                    self.syms.setdefault(name, []).append((shndx, value, info >> 4))

    def cstr(self, off):
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")

    def file_at(self, vaddr, size):
        for p in self.phdrs:
            if p["type"] == 1 and p["vaddr"] <= vaddr and vaddr + size <= p["vaddr"] + p["filesz"]:
                return p["offset"] + vaddr - p["vaddr"]
        raise ValueError("address 0x%x is not in a loaded segment" % vaddr)


def defined(elf, name):
    """The single defined global (strong) or weak definition of NAME."""
    defs = [d for d in elf.syms.get(name, []) if d[0] != SHN_UNDEF]
    if len(defs) != 1:
        return None
    return defs[0]


def main():
    if len(sys.argv) != 6 or sys.argv[1] not in ("object", "linked"):
        print(__doc__.strip().splitlines()[2])
        return 2
    mode, path, expected, start_fn, stop_fn = sys.argv[1:]
    try:
        elf = Elf(open(path, "rb").read())
    except (OSError, ValueError, IndexError, struct.error) as e:
        print("cannot read %s: %s" % (path, e))
        return 2
    problems = []

    idents = [s for s in elf.shdrs if s["name"] == IDENT]
    if len(idents) != 1:
        problems.append("%d %s sections, expected 1" % (len(idents), IDENT))
    else:
        s = idents[0]
        if s["type"] != SHT_PROGBITS:
            problems.append("%s has type %d, expected PROGBITS" % (IDENT, s["type"]))
        if s["flags"] & SHF_ALLOC:
            problems.append("%s is loaded (SHF_ALLOC set)" % IDENT)
        got = elf.data[s["offset"]:s["offset"] + s["size"]].hex()
        if got != expected:
            problems.append("identity record %s, expected %s" % (got, expected))

    for entry, fn in (("module_start", start_fn), ("module_stop", stop_fn)):
        e, f = defined(elf, entry), defined(elf, fn)
        if e is None or f is None:
            problems.append("%s=%s %s=%s (need one definition each)"
                            % (entry, elf.syms.get(entry), fn, elf.syms.get(fn)))
        elif (e[0], e[1]) != (f[0], f[1]):
            problems.append("%s is section %d + 0x%x, %s is section %d + 0x%x"
                            % (entry, e[0], e[1], fn, f[0], f[1]))
        elif e[2] != 1:
            problems.append("%s is weak; the default was not replaced" % entry)

    if mode == "linked":
        try:
            info = defined(elf, "__sys_prx_module_info")
            if info is None:
                raise ValueError("no __sys_prx_module_info")
            mi = elf.file_at(info[1], 52)
            ex_start, ex_end = struct.unpack_from(">II", elf.data, mi + 0x24)
            mgmt = []
            off = ex_start
            while off < ex_end:
                r = elf.file_at(off, 44)
                size = elf.data[r] or 44
                attrs, nfunc = struct.unpack_from(">HH", elf.data, r + 4)
                nids, addrs = struct.unpack_from(">II", elf.data, r + 0x14)
                if attrs & 0x8000 and not attrs & 0x0001:
                    n = struct.unpack_from(">%dI" % nfunc, elf.data, elf.file_at(nids, 4 * nfunc))
                    a = struct.unpack_from(">%dI" % nfunc, elf.data, elf.file_at(addrs, 4 * nfunc))
                    mgmt.append(dict(zip(n, a)))
                off += size
            if len(mgmt) != 1:
                problems.append("%d management records, expected 1" % len(mgmt))
            else:
                for nid, fn in ((NID_START, start_fn), (NID_STOP, stop_fn)):
                    f = defined(elf, fn)
                    want = f[1] if f else None
                    if mgmt[0].get(nid) != want:
                        problems.append("management NID 0x%08x -> %s, %s is %s"
                                        % (nid, hex(mgmt[0].get(nid) or 0), fn,
                                           hex(want) if want is not None else None))
        except (ValueError, IndexError, struct.error) as e:
            problems.append("management record: %s" % e)

    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
