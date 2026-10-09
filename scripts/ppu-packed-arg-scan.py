#!/usr/bin/env python3
"""ppu-packed-arg-scan: find PPU code that addresses memory through a packed
argument register (32-bit pointer ABI).

A struct of 8 bytes or less passed by value (std::string_view, std::span, a
{pointer, length} pair) arrives packed in one 64-bit GPR.  A function that
splits it reads the high member with a 32-bit right shift.  If the raw
register, or a plain copy of it (mr), is then used as the base or index of a
load or store, the effective address carries the other member in bits 32-63:
a data storage fault on a PS3, a crash or a silently truncated access in
RPCS3's PPU recompiler, and "Narrowing error" in its PPU interpreter.

Until the compiler canonicalises such pointers, pass these structs by const
reference on the PS3 path.

The scan is linear within each function and only sees functions that read
the high member with sradi/srdi 32; a clean result is not a proof.

usage: ppu-packed-arg-scan.py [--objdump PATH] FILE...
FILE is a linked PPU ELF (functions found through .opd), a relocatable
object or an archive (functions found through their symbols).

Exit status: 0 no hits, 1 hits, 2 usage or tool error.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

ARG_REGS = {"r%d" % n for n in range(3, 11)}
CALL_CLOBBERED = {"r%d" % n for n in [0] + list(range(3, 13))}
NO_WRITE = re.compile(r"^(st|cmp|b|tw|td|mt|dcb|icb|sync|isync|eieio|lwsync|nop|cr)")
INSN = re.compile(r"^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$")
HEADER = re.compile(r"^([0-9a-f]+) <(.+)>:$")
MEMBER = re.compile(r"^(\S+\.o):\s+file format")
D_FORM = re.compile(r"^(\S+),\s*(-?\w+)\((r\d+)\)$")
X_FORM = re.compile(r"^(l|st)\w*x$")


def find_objdump(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("powerpc64-ps3-elf-objdump", "ppu-objdump"):
        for d in (here, os.path.join(here, "..", "ppu", "bin")):
            for ext in ("", ".exe"):
                p = os.path.join(d, name + ext)
                if os.path.isfile(p):
                    return p
        p = shutil.which(name)
        if p:
            return p
    return None


def linked_functions(data):
    """Entry address -> name, from a linked ELF's .opd descriptors.
    Empty for anything that is not a big-endian ELF64 executable."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        return {}
    if struct.unpack(">H", data[16:18])[0] != 2:        # ET_EXEC
        return {}
    shoff = struct.unpack(">Q", data[0x28:0x30])[0]
    shentsize, shnum, shstrndx = struct.unpack(">HHH", data[0x3a:0x40])
    secs = [struct.unpack(">IIQQQQIIQQ", data[shoff + i * shentsize:shoff + i * shentsize + 64])
            for i in range(shnum)]
    stroff = secs[shstrndx][4]

    def cstr(off):
        return data[off:data.index(b"\0", off)].decode(errors="replace")

    by = {cstr(stroff + s[0]): s for s in secs}
    opd = by.get(".opd")
    if not opd:
        return {}
    desc = {}
    for k in range(0, opd[5], 8):
        desc[opd[3] + k] = struct.unpack(">I", data[opd[4] + k:opd[4] + k + 4])[0]
    names = {}
    sym = by.get(".symtab")
    if sym:
        strtab = secs[sym[6]][4]
        for k in range(0, sym[5], 24):
            n, _info, _other, _shndx, value, _size = struct.unpack(
                ">IBBHQQ", data[sym[4] + k:sym[4] + k + 24])
            if value in desc:
                names[value] = cstr(strtab + n)
    return {entry: names.get(addr, "0x%x" % entry) for addr, entry in desc.items()}


def scan(objdump, path):
    with open(path, "rb") as f:
        data = f.read()
    entries = linked_functions(data)
    by_header = not entries
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError("%s failed on %s: %s" % (objdump, path, out.stderr.strip()))

    hits = []
    fn = None
    packed = set()
    tainted = {}
    obj = ""

    def begin(name):
        nonlocal fn, packed, tainted
        fn = name
        packed = set()
        tainted = {r: r for r in ARG_REGS}

    for line in out.stdout.splitlines():
        m = MEMBER.match(line)
        if m:
            obj = "" if m.group(1) == path else m.group(1) + ":"
            fn = None
            continue
        if by_header:
            m = HEADER.match(line)
            if m:
                begin(obj + m.group(2))
                continue
        m = INSN.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        op = m.group(2)
        args = m.group(3).split("#")[0].split("<")[0].strip()
        if not by_header and addr in entries:
            begin(entries[addr])
        if fn is None:
            continue
        ops = [a.strip() for a in args.split(",")] if args else []

        if op in ("sradi", "srdi") and len(ops) == 3 and ops[2] == "32" and ops[1] in tainted:
            packed.add(tainted[ops[1]])

        bases = []
        dm = D_FORM.match(args)
        if (op.startswith("l") or op.startswith("st")) and dm:
            bases = [dm.group(3)]
        elif X_FORM.match(op) and len(ops) == 3:
            bases = ops[1:]
        for b in bases:
            if b in tainted and tainted[b] in packed:
                hits.append((fn, addr, "%s %s" % (op, args), tainted[b]))

        if op == "mr" and len(ops) == 2:
            if ops[1] in tainted:
                tainted[ops[0]] = tainted[ops[1]]
            else:
                tainted.pop(ops[0], None)
        elif ops and not NO_WRITE.match(op) and re.match(r"^r\d+$", ops[0]):
            tainted.pop(ops[0], None)
        if op.startswith("bl") and op not in ("blr", "blrl"):
            for r in CALL_CLOBBERED:
                tainted.pop(r, None)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--objdump", help="PPU objdump (default: next to this script, then PATH)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    objdump = find_objdump(a.objdump)
    if not objdump:
        print("ppu-packed-arg-scan: no powerpc64-ps3-elf-objdump found; pass --objdump", file=sys.stderr)
        return 2
    total = 0
    for path in a.files:
        try:
            hits = scan(objdump, path)
        except (OSError, RuntimeError) as e:
            print("ppu-packed-arg-scan: %s" % e, file=sys.stderr)
            return 2
        for fn, addr, insn, reg in hits:
            print("%s: %s at 0x%x: %s (raw %s)" % (path, fn, addr, insn, reg))
        total += len(hits)
    print("ppu-packed-arg-scan: %d hit(s)" % total, file=sys.stderr)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
