"""
Synthetic PPU links for tests/host/toc-linked-check-test.py.

Writes big-endian ELF64 PowerPC objects and archives, links them with a
small model linker (placement, symbol resolution, the TOC-relative and
address relocations the checker models, optional linker rewrites) and
emits the executable, a GNU-ld-shaped map, an ld -t -t trace and a
toc-linked-check manifest.  Tests then doctor the outputs to build the
negative cases.  Only what the checker reads is modelled; this is not a
general linker.
"""

import hashlib
import json
import os
import struct

NL = chr(10)

R = {
    'R_PPC64_ADDR32': 1, 'R_PPC64_ADDR16': 3, 'R_PPC64_REL24': 10, 'R_PPC64_REL14': 11,
    'R_PPC64_GOT16': 14, 'R_PPC64_REL32': 26, 'R_PPC64_ADDR64': 38,
    'R_PPC64_TOC16': 47, 'R_PPC64_TOC16_LO': 48, 'R_PPC64_TOC16_HI': 49,
    'R_PPC64_TOC16_HA': 50, 'R_PPC64_TOC': 51, 'R_PPC64_TOC16_DS': 63,
    'R_PPC64_TOC16_LO_DS': 64,
}

SHT = {'NULL': 0, 'PROGBITS': 1, 'SYMTAB': 2, 'STRTAB': 3, 'RELA': 4, 'NOBITS': 8}
SHF = {'W': 1, 'A': 2, 'X': 4, 'M': 0x10, 'S': 0x20, 'I': 0x40, 'G': 0x200}
STB = {'LOCAL': 0, 'GLOBAL': 1, 'WEAK': 2}
STT = {'NOTYPE': 0, 'OBJECT': 1, 'FUNC': 2, 'SECTION': 3}


def flags_of(text):
    v = 0
    for c in text:
        v |= SHF[c]
    return v


# ---------------------------------------------------------------------------
# Instruction encoders (D and DS forms the fixtures need)

def addis(rt, ra, imm=0):
    return (15 << 26) | (rt << 21) | (ra << 16) | (imm & 0xffff)


def addi(rt, ra, imm=0):
    return (14 << 26) | (rt << 21) | (ra << 16) | (imm & 0xffff)


def lwz(rt, ra, d=0):
    return (32 << 26) | (rt << 21) | (ra << 16) | (d & 0xffff)


def stw(rs, ra, d=0):
    return (36 << 26) | (rs << 21) | (ra << 16) | (d & 0xffff)


def ld(rt, ra, d=0):
    return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xfffc)


def li(rt, imm):
    return addi(rt, 0, imm)


def b(rel):
    return (18 << 26) | (rel & 0x3fffffc)


def bl(rel=0):
    return (18 << 26) | (rel & 0x3fffffc) | 1


def beq(rel):
    # bc 12,2,rel
    return (16 << 26) | (12 << 21) | (2 << 16) | (rel & 0xfffc)


def bctr():
    return 0x4e800420


def mtctr(rs):
    return 0x7c0903a6 | (rs << 21)


def bcctr(bo, bi):
    return (19 << 26) | (bo << 21) | (bi << 16) | (528 << 1)


def cmplwi(ra, ui, cr=0):
    return (10 << 26) | (cr << 23) | (ra << 16) | (ui & 0xffff)


def bc(bo, bi, rel):
    return (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xfffc)


def bgt(rel, cr=0):
    # bc 12,4*cr+1,rel
    return (16 << 26) | (12 << 21) | ((4 * cr + 1) << 16) | (rel & 0xfffc)


def sldi(ra, rs, n):
    me = 63 - n
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((n & 31) << 11)
            | ((((me & 31) << 1) | (me >> 5)) << 5) | (1 << 2) | ((n >> 5) << 1))


def cmpldi(ra, ui, cr=0):
    return cmplwi(ra, ui, cr) | (1 << 21)


def rldic(ra, rs, sh, mb):
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11)
            | ((((mb & 31) << 1) | (mb >> 5)) << 5) | (2 << 2) | ((sh >> 5) << 1))


def clrlwi(ra, rs, n):
    # rlwinm ra,rs,0,n,31
    return (21 << 26) | (rs << 21) | (ra << 16) | (n << 6) | (31 << 1)


def twi_lgt(ra, si):
    # twi 1,ra,si: trap if ra >u si
    return (3 << 26) | (1 << 21) | (ra << 16) | (si & 0xffff)


def lbz(rt, ra, d=0):
    return (34 << 26) | (rt << 21) | (ra << 16) | (d & 0xffff)


def x_form(xo, rt, ra, rb):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1)


def lwax(rt, ra, rb):
    return x_form(341, rt, ra, rb)


def add(rt, ra, rb):
    return x_form(266, rt, ra, rb)


def lswi(rt, ra, nb):
    return x_form(597, rt, ra, nb & 31)


def lswx(rt, ra, rb):
    return x_form(533, rt, ra, rb)


def stfdux(frs, ra, rb):
    return x_form(759, frs, ra, rb)


BLR = 0x4e800020
NOP = 0x60000000


def code(*words):
    return b''.join(struct.pack('>I', w & 0xffffffff) for w in words)


# ---------------------------------------------------------------------------
# ELF writer

class Section:
    def __init__(self, name, stype, flags, data, size, align, entsize, addr=0):
        self.name, self.type, self.flags = name, stype, flags
        self.data, self.size, self.align, self.entsize = data, size, align, entsize
        self.addr = addr


def _strtab(names):
    blob = bytearray(b'\0')
    offs = {}
    for n in names:
        if n in offs:
            continue
        if n == '':
            offs[n] = 0
            continue
        offs[n] = len(blob)
        blob += n.encode() + b'\0'
    return bytes(blob), offs


def write_elf(etype, sections, symbols, relas, entry=0):
    """
    sections: [Section]  (index 1..n in order)
    symbols:  [(name, value, size, bind, type, shndx)] without the null symbol;
              locals must come first.  shndx: int, 'UND' or 'ABS'.
    relas:    {section index: [(offset, type, symidx, addend)]}
    Returns ELF bytes.
    """
    secs = list(sections)
    nsec = len(secs)
    # Extra sections: .rela*, .symtab, .strtab, .shstrtab
    rela_secs = []
    for target in sorted(relas):
        rows = relas[target]
        blob = b''.join(struct.pack('>QQq', off, (sym << 32) | t, add) for off, t, sym, add in rows)
        rela_secs.append((target, Section('.rela' + secs[target - 1].name, 'RELA', 'I', blob, len(blob), 8, 24)))
    sym_names = [s[0] for s in symbols]
    strtab, stroffs = _strtab(sym_names)
    first_global = 1 + sum(1 for s in symbols if s[3] == 'LOCAL')
    symblob = bytearray(24)
    for name, value, size, bind, stype, shndx in symbols:
        nd = {'UND': 0, 'ABS': 0xfff1}.get(shndx, shndx)
        symblob += struct.pack('>IBBHQQ', stroffs[name], (STB[bind] << 4) | STT[stype], 0, nd, value, size)
    symtab_idx = nsec + len(rela_secs) + 1
    all_secs = secs + [s for _, s in rela_secs]
    all_secs.append(Section('.symtab', 'SYMTAB', '', bytes(symblob), len(symblob), 8, 24))
    all_secs.append(Section('.strtab', 'STRTAB', '', strtab, len(strtab), 1, 0))
    shnames = [s.name for s in all_secs] + ['.shstrtab']
    shstr, shoffs = _strtab(shnames)
    all_secs.append(Section('.shstrtab', 'STRTAB', '', shstr, len(shstr), 1, 0))
    links = {}
    infos = {}
    for k, (target, _) in enumerate(rela_secs):
        idx = nsec + 1 + k
        links[idx] = symtab_idx
        infos[idx] = target
    links[symtab_idx] = symtab_idx + 1
    infos[symtab_idx] = first_global

    out = bytearray(64)
    offsets = []
    for s in all_secs:
        if s.type == 'NOBITS':
            offsets.append(len(out))
            continue
        al = max(s.align, 1)
        while len(out) % al:
            out += b'\0'
        offsets.append(len(out))
        out += s.data
    while len(out) % 8:
        out += b'\0'
    shoff = len(out)
    out += bytes(64)
    for k, s in enumerate(all_secs):
        idx = k + 1
        out += struct.pack('>IIQQQQIIQQ', shoffs[s.name], SHT[s.type], flags_of(s.flags), s.addr,
                           offsets[k], s.size, links.get(idx, 0), infos.get(idx, 0), s.align, s.entsize)
    ident = b'\x7fELF' + bytes([2, 2, 1, 0]) + bytes(8)
    out[0:64] = ident + struct.pack('>HHIQQQIHHHHHH', etype, 21, 1, entry, 0, shoff, 0, 64, 0, 0, 64,
                                    len(all_secs) + 1, len(all_secs))
    return bytes(out)


# ---------------------------------------------------------------------------
# Relocatable objects

class Obj:
    """A relocatable object under construction."""

    def __init__(self, name):
        self.name = name
        self.sections = []     # Section
        self.symbols = []      # dict(name, value, size, bind, type, sec)
        self.relocs = {}       # section name -> [(off, type, symname, addend)]

    def section(self, name, data=b'', flags='AX', stype='PROGBITS', align=4, entsize=0, size=None):
        if stype == 'NOBITS':
            self.sections.append(Section(name, stype, flags, b'', size or 0, align, entsize))
        else:
            self.sections.append(Section(name, stype, flags, bytes(data), len(data), align, entsize))
        self.symbols.append({'name': name, 'value': 0, 'size': 0, 'bind': 'LOCAL',
                             'type': 'SECTION', 'sec': len(self.sections), 'secsym': True})
        return len(self.sections)

    def sym(self, name, sec, value=0, bind='LOCAL', stype='NOTYPE', size=0):
        """sec: a section name of this object, 'UND' or 'ABS'."""
        if sec not in ('UND', 'ABS'):
            sec = self.sec_index(sec)
        self.symbols.append({'name': name, 'value': value, 'size': size, 'bind': bind,
                             'type': stype, 'sec': sec, 'secsym': False})

    def rel(self, secname, off, rtype, symname, addend=0):
        self.relocs.setdefault(secname, []).append((off, rtype, symname, addend))

    def sec_index(self, name):
        if isinstance(name, int):
            return name
        hits = [k + 1 for k, s in enumerate(self.sections) if s.name == name]
        if len(hits) != 1:
            raise ValueError(f"{self.name}: {len(hits)} sections named {name}")
        return hits[0]

    def ordered_symbols(self):
        locs = [s for s in self.symbols if s['bind'] == 'LOCAL']
        globs = [s for s in self.symbols if s['bind'] != 'LOCAL']
        return locs + globs

    def sym_index(self, name):
        order = self.ordered_symbols()
        hits = [k + 1 for k, s in enumerate(order) if s['name'] == name and not s['secsym']]
        if not hits:
            hits = [k + 1 for k, s in enumerate(order) if s['name'] == name]
        if len(hits) != 1:
            raise ValueError(f"{self.name}: {len(hits)} symbols named {name}")
        return hits[0]

    def to_bytes(self):
        order = self.ordered_symbols()
        syms = [('' if s['secsym'] else s['name'], s['value'], s['size'], s['bind'], s['type'], s['sec'])
                for s in order]
        relas = {}
        for secname, rows in self.relocs.items():
            idx = self.sec_index(secname)
            relas[idx] = [(off, R[t], self.sym_index(sn), add) for off, t, sn, add in rows]
        return write_elf(1, self.sections, syms, relas)


def write_archive(path, members):
    """members: [(name, bytes)]; names up to 15 characters."""
    out = bytearray(b'!<arch>\n')
    for name, data in members:
        hdr = f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{644:<8}{len(data):<10}`\n".encode()
        assert len(hdr) == 60
        out += hdr + data
        if len(data) % 2:
            out += b'\n'
    with open(path, 'wb') as f:
        f.write(out)


# ---------------------------------------------------------------------------
# The model linker

def ha16(v):
    return ((v + 0x8000) >> 16) & 0xffff


class Link:
    """
    inputs: [(Obj, file_path, member or None)] in link order.
    Place every input section (unless discarded), resolve symbols, apply
    relocations and write the executable, map, trace and manifest into
    outdir.  Attributes after run(): elf_path, map_text, out sections,
    placement {(obj name, section name): address}.
    """

    TEXT_BASE = 0x10000

    def __init__(self, inputs, discard=(), toc_optimize=False, opd=(), extra_out=()):
        self.inputs = inputs
        self.discard = set(discard)          # {(obj name, section name)}
        self.toc_optimize = toc_optimize     # model ld's addis->nop and ld->addi edits
        self.opd = list(opd)                 # [(obj name, symbol)] compact descriptors
        self.extra_out = list(extra_out)     # [Section] appended to the executable
        self.out_relas = {}                  # {executable section index: [(off, type, sym, addend)]}

    def out_kind(self, sec):
        if sec.name == '.toc':
            return '.toc'
        if 'X' in sec.flags:
            return '.text'
        if sec.type == 'NOBITS':
            return '.bss'
        if 'W' in sec.flags:
            return '.data'
        return '.rodata'

    def run(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        order = ['.text', '.rodata', '.opd', '.toc', '.data', '.bss']
        contents = {k: [] for k in order}
        for obj, _, _ in self.inputs:
            for k, s in enumerate(obj.sections):
                if (obj.name, s.name) in self.discard:
                    continue
                contents[self.out_kind(s)].append((obj, k + 1, s))
        addr = self.TEXT_BASE
        self.out = {}
        self.place = {}
        self.place_idx = {}
        for kind in order:
            addr = (addr + 0xff) & ~0xff
            start = addr
            if kind == '.opd':
                addr += 8 * len(self.opd)
            for obj, idx, s in contents[kind]:
                al = max(s.align, 1)
                addr = (addr + al - 1) // al * al
                self.place[(obj.name, s.name)] = addr
                self.place_idx[(obj.name, idx)] = addr
                addr += s.size
            self.out[kind] = [start, addr - start]
        if self.out['.toc'][1] == 0:
            self.out['.toc'][1] = 0
        self.toc = self.out['.toc'][0] + 0x8000
        # Images of every output section.
        img = {k: bytearray(self.out[k][1]) for k in order if k != '.bss'}
        for kind in order:
            if kind == '.bss':
                continue
            for obj, idx, s in contents[kind]:
                o = self.place_idx[(obj.name, idx)] - self.out[kind][0]
                img[kind][o:o + s.size] = s.data
        # Global symbols.
        self.globals = {}
        for obj, _, _ in self.inputs:
            for s in obj.symbols:
                if s['bind'] != 'LOCAL' and s['sec'] not in ('UND',):
                    if (obj.name, s['sec']) in self.place_idx:
                        self.globals[s['name']] = self.place_idx[(obj.name, s['sec'])] + s['value']

        def S(obj, symname):
            k = obj.sym_index(symname)
            s = obj.ordered_symbols()[k - 1]
            if s['bind'] == 'LOCAL':
                if s['sec'] == 'ABS':
                    return s['value']
                return self.place_idx[(obj.name, s['sec'])] + s['value']
            if s['name'] in self.globals:
                return self.globals[s['name']]
            if s['bind'] == 'WEAK':
                return 0
            raise ValueError(f"undefined {s['name']}")

        def put(kind, a, width, v):
            o = a - self.out[kind][0]
            img[kind][o:o + width] = (v & ((1 << (8 * width)) - 1)).to_bytes(width, 'big')

        def get(kind, a, width):
            o = a - self.out[kind][0]
            return int.from_bytes(img[kind][o:o + width], 'big')

        self.applied = []
        for obj, _, _ in self.inputs:
            for secname, rows in obj.relocs.items():
                idx = obj.sec_index(secname)
                if (obj.name, idx) not in self.place_idx:
                    continue
                kind = self.out_kind(obj.sections[idx - 1])
                base = self.place_idx[(obj.name, idx)]
                for off, t, sn, add in rows:
                    v = S(obj, sn) + add
                    a = base + off
                    rel = v - self.toc
                    if t == 'R_PPC64_ADDR64':
                        put(kind, a, 8, v)
                    elif t == 'R_PPC64_ADDR32':
                        put(kind, a, 4, v)
                    elif t == 'R_PPC64_TOC':
                        put(kind, a, 8, self.toc)
                    elif t in ('R_PPC64_TOC16', 'R_PPC64_TOC16_LO'):
                        put(kind, a, 2, rel)
                    elif t in ('R_PPC64_TOC16_DS', 'R_PPC64_TOC16_LO_DS'):
                        put(kind, a, 2, (rel & ~3) | (get(kind, a, 2) & 3))
                    elif t == 'R_PPC64_TOC16_HA':
                        put(kind, a, 2, ha16(rel))
                    elif t == 'R_PPC64_TOC16_HI':
                        put(kind, a, 2, rel >> 16)
                    elif t == 'R_PPC64_REL14':
                        put(kind, a, 4, (get(kind, a, 4) & ~0xfffc) | ((v - a) & 0xfffc))
                    elif t == 'R_PPC64_REL24':
                        put(kind, a, 4, (get(kind, a, 4) & ~0x3fffffc) | ((v - a) & 0x3fffffc))
                    else:
                        continue
                    self.applied.append((obj.name, secname, off, t, a))
        # The .opd image.
        for k, (oname, symname) in enumerate(self.opd):
            obj = [o for o, _, _ in self.inputs if o.name == oname][0]
            put('.opd', self.out['.opd'][0] + 8 * k, 4, S(obj, symname))
            put('.opd', self.out['.opd'][0] + 8 * k + 4, 4, self.toc)
        self.img = img
        self.write(outdir, order, img)
        return self

    # -- doctoring helpers -------------------------------------------------
    def kind_at(self, a):
        for k, (start, size) in self.out.items():
            if k != '.bss' and start <= a < start + size:
                return k
        raise ValueError(f"0x{a:x} not in an output section")

    def word(self, a):
        k = self.kind_at(a)
        o = a - self.out[k][0]
        return int.from_bytes(self.img[k][o:o + 4], 'big')

    def poke(self, a, data):
        k = self.kind_at(a)
        o = a - self.out[k][0]
        self.img[k][o:o + len(data)] = data

    def poke_word(self, a, w):
        self.poke(a, struct.pack('>I', w & 0xffffffff))

    def rewrite(self):
        """Rewrite the executable after doctoring; refresh the manifest's pins."""
        self.write(self.outdir, self.order, self.img)

    def write(self, outdir, order, img):
        self.outdir, self.order = outdir, order
        secs = []
        sec_idx = {}
        for kind in order:
            start, size = self.out[kind]
            if kind == '.bss':
                secs.append(Section(kind, 'NOBITS', 'WA', b'', size, 8, 0, start))
            else:
                fl = {'.text': 'AX', '.rodata': 'A', '.opd': 'WA', '.toc': 'WA', '.data': 'WA'}[kind]
                secs.append(Section(kind, 'PROGBITS', fl, bytes(img[kind]), size, 8, 0, start))
            sec_idx[kind] = len(secs)
        for s in self.extra_out:
            secs.append(s)
        syms = [('.TOC.', self.toc, 0, 'LOCAL', 'NOTYPE', sec_idx['.toc'])]
        for name, v in sorted(self.globals.items()):
            k = self.kind_at(v) if v < self.out['.bss'][0] else '.bss'
            syms.append((name, v, 0, 'GLOBAL', 'NOTYPE', sec_idx[k]))
        self.elf_bytes = write_elf(2, secs, syms, self.out_relas)
        self.elf_path = os.path.join(outdir, 'out.elf')
        with open(self.elf_path, 'wb') as f:
            f.write(self.elf_bytes)
        self.write_map(outdir)
        self.write_trace(outdir)
        self.write_manifest(outdir)

    def file_key(self, path):
        return os.path.abspath(path).replace(chr(92), '/')

    def input_label(self, obj, path, member):
        return f"{self.file_key(path)}({member})" if member else self.file_key(path)

    def write_map(self, outdir, mutate=None):
        lines = []
        members = [(o, p, m) for o, p, m in self.inputs if m]
        if members:
            lines.append('Archive member included to satisfy reference by file (symbol)')
            lines.append('')
            for o, p, m in members:
                lines.append(f"{self.input_label(o, p, m)}")
                lines.append(f"                              {self.file_key(self.inputs[0][1])} (sym)")
            lines.append('')
        disc = [(o, p, m, s) for o, p, m in self.inputs for s in o.sections if (o.name, s.name) in self.discard]
        if disc:
            lines.append('Discarded input sections')
            lines.append('')
            for o, p, m, s in disc:
                lines.append(f" {s.name:<15}0x{0:016x}{s.size:>#11x} {self.input_label(o, p, m)}")
            lines.append('')
        else:
            lines.append('There are no discarded input sections')
            lines.append('')
        lines += ['Memory Configuration', '', 'Name             Origin             Length             Attributes',
                  '*default*        0x0000000000000000 0xffffffffffffffff', '', 'Linker script and memory map', '']
        for kind in self.order:
            start, size = self.out[kind]
            lines.append(f"{kind:<15}0x{start:016x}{size:>#11x}")
            for o, p, m in self.inputs:
                for k, s in enumerate(o.sections):
                    key = (o.name, k + 1)
                    if key not in self.place_idx or self.out_kind(s) != kind:
                        continue
                    a = self.place_idx[key]
                    if len(s.name) > 14:
                        lines.append(f" {s.name}")
                        lines.append(f"{'':<16}0x{a:016x}{s.size:>#11x} {self.input_label(o, p, m)}")
                    else:
                        lines.append(f" {s.name:<15}0x{a:016x}{s.size:>#11x} {self.input_label(o, p, m)}")
            lines.append('')
        lines.append(f"OUTPUT({self.file_key(os.path.join(outdir, 'out.elf'))} elf64-powerpc)")
        self.map_lines = lines
        self.map_path = os.path.join(outdir, 'link.map')
        with open(self.map_path, 'w', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')

    def write_trace(self, outdir):
        lines = []
        seen = set()
        for o, p, m in self.inputs:
            if m:
                if p not in seen:
                    lines.append(self.file_key(p))
                    seen.add(p)
                lines.append(f"({self.file_key(p)}){m}")
            elif p not in seen:
                lines.append(self.file_key(p))
                seen.add(p)
        self.trace_path = os.path.join(outdir, 'link.trace')
        with open(self.trace_path, 'w', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')

    def write_manifest(self, outdir):
        def sha(p):
            with open(p, 'rb') as f:
                return hashlib.sha256(f.read()).hexdigest()
        inputs = []
        seen = set()
        for o, p, m in self.inputs:
            if p in seen:
                continue
            seen.add(p)
            inputs.append({'key': self.file_key(p), 'file': self.file_key(p),
                           'kind': 'archive' if m else 'object', 'sha256': sha(p)})
        man = {'format': 'toc-linked-check-manifest-1', 'cwd': '.', 'argv': ['model-linker'],
               'elf': {'file': 'out.elf', 'sha256': sha(self.elf_path)},
               'map': {'file': 'link.map', 'sha256': sha(self.map_path)},
               'trace': {'file': 'link.trace', 'sha256': sha(self.trace_path)},
               'inputs': inputs}
        self.manifest_path = os.path.join(outdir, 'manifest.json')
        with open(self.manifest_path, 'w', newline='\n') as f:
            json.dump(man, f, indent=1, sort_keys=True)
            f.write('\n')

    def out_index(self, kind):
        return self.order.index(kind) + 1

    def patch_shdr(self, name, field_off, width, value):
        """Patch one field of a named section header in the written executable."""
        data = bytearray(self.elf_bytes)
        shoff = int.from_bytes(data[40:48], 'big')
        shnum = int.from_bytes(data[60:62], 'big')
        shstrndx = int.from_bytes(data[62:64], 'big')
        so = shoff + 64 * shstrndx
        stroff = int.from_bytes(data[so + 24:so + 32], 'big')
        hit = 0
        for i in range(shnum):
            o = shoff + 64 * i
            n = int.from_bytes(data[o:o + 4], 'big')
            end = data.index(0, stroff + n)
            if data[stroff + n:end].decode() == name:
                data[o + field_off:o + field_off + width] = value.to_bytes(width, 'big')
                hit += 1
        if hit != 1:
            raise ValueError(f"{hit} sections named {name}")
        self.elf_bytes = bytes(data)
        with open(self.elf_path, 'wb') as f:
            f.write(self.elf_bytes)
        self.repin()

    def edit_map(self, fn):
        """Rewrite the map through fn(lines) -> lines and re-pin it."""
        self.map_lines = fn(list(self.map_lines))
        with open(self.map_path, 'w', newline=NL) as f:
            f.write(NL.join(self.map_lines) + NL)
        self.repin()

    def repin(self):
        """Re-pin the manifest after a test edits the map, trace or an input."""
        self.write_manifest(self.outdir)
