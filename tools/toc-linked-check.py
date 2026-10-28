#!/usr/bin/env python3
"""
Linked-image TOC access checker for PPU executables (v1).

Proves, for one linked PPU ELF, that every TOC-relative access the input
objects describe still reaches what the compiler asked for after the link:
a load from a .toc entry reads that entry's bytes, and an address formed
or a datum loaded relative to the TOC hits the right symbol.  The ground
truth comes from the input objects, not from the output's own (possibly
consistently wrong) relocations, so a linker that moves an access and its
emitted relocation together is still caught.

  capture --out DIR -- <link command...>
      Run the link with a map (-Wl,-Map) and the input trace (-Wl,-t -t),
      then pin the ELF, the map, the trace, the command line and every
      input file by SHA-256 in DIR/manifest.json.

  check DIR/manifest.json
      Re-hash everything the manifest pins, place each input code section
      through the map, decode every admitted access in the ELF and compare
      it with what the objects require.

Scope of v1 (anything outside it is refused, never skipped):
  - one TOC: every .opd descriptor carries the same TOC word as .TOC.,
    and no linker stub section (.stub, .glink) has contents;
  - compact 8-byte .opd descriptors (4-byte code address, 4-byte TOC);
  - access relocations R_PPC64_TOC16, _LO, _HA, _DS and _LO_DS on D/DS-form
    loads, stores and addi; other TOC- or GOT-relative relocation types
    are inventoried and refused;
  - linker rewrites modelled: addis rX,r2,0 -> nop with the paired
    instruction rebased on r2, ld -> addi of the entry's own value, and
    nop -> TOC restore after a call;
  - .toc entries written by R_PPC64_ADDR64, R_PPC64_ADDR32 or R_PPC64_TOC,
    and literal .toc bytes; other entry relocations are refused.

Success means every admitted access described by the inputs was verified;
it is not a claim about other TOC uses or about runtime behaviour.
Exit status: 0 PASS (verified > 0, nothing failed, refused or
unaccounted), 1 FAIL or INCOMPLETE, 2 bad or unsupported input.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import posixpath
import re
import subprocess
import sys

_spec = importlib.util.spec_from_file_location(
    'ppc64_elf_parse', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ppc64_elf_parse.py'))
_parse = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_parse)

MANIFEST_FORMAT = 'toc-linked-check-manifest-1'

SUPPORTED_ACCESS = {
    'R_PPC64_TOC16': 'single',
    'R_PPC64_TOC16_DS': 'single',
    'R_PPC64_TOC16_LO': 'lo',
    'R_PPC64_TOC16_LO_DS': 'lo',
    'R_PPC64_TOC16_HA': 'ha',
}
DS_RELOCS = {'R_PPC64_TOC16_DS', 'R_PPC64_TOC16_LO_DS'}

NOP = 0x60000000
# TOC restores ld may put in the nop after a call.
TOC_RESTORES = {0xe8410028, 0x80410014}

# Primary opcode -> (kind, width) for the instruction an access relocation
# sits on.  kind: load, store, addr (addi).  Update forms are refused.
D_FORMS = {
    14: ('addr', None),
    32: ('load', 4), 34: ('load', 1), 40: ('load', 2), 42: ('load', 2),
    48: ('load', 4), 50: ('load', 8),
    36: ('store', 4), 38: ('store', 1), 44: ('store', 2),
    52: ('store', 4), 54: ('store', 8),
}
DS_FORMS = {
    (58, 0): ('load', 8),   # ld
    (58, 2): ('load', 4),   # lwa
    (62, 0): ('store', 8),  # std
}


class InputError(Exception):
    """Bad or unsupported input: exit 2."""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def sext16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def be(data, off, width):
    return int.from_bytes(data[off:off + width], 'big')


def norm_name(name):
    """The identity key of a path as ld prints it in the trace and the map."""
    s = name.strip().replace(chr(92), '/')
    drive = ''
    m = re.match(r'^([A-Za-z]:)(/.*)$', s)
    if m:
        drive, s = m.group(1).upper(), m.group(2)
    return drive + posixpath.normpath(s)


def resolve_path(key, cwd):
    if re.match(r'^[A-Za-z]:/', key) or key.startswith('/'):
        return key
    return posixpath.join(cwd.replace(chr(92), '/'), key)


def split_member(text):
    """'archive(member)' -> (archive, member); anything else -> (text, None)."""
    m = re.match(r'^(.*)\(([^()]+)\)$', text)
    if m:
        return m.group(1), m.group(2)
    return text, None


# ---------------------------------------------------------------------------
# Instruction decoding

def gpr_writes(word):
    """
    GPRs an instruction may write, for the HA/LO continuity check.
    Returns a set of register numbers, or None when the form is not known
    (the caller treats that as a possible clobber of any register).
    """
    op = word >> 26
    rt = (word >> 21) & 31
    ra = (word >> 16) & 31
    volatile = {0} | set(range(3, 13))
    if op in (7, 8, 12, 13, 14, 15, 32, 34, 40, 42):
        return {rt}
    if op in (33, 35, 41, 43):
        return {rt, ra}
    if op == 46:
        return set(range(rt, 32))
    if op in (24, 25, 26, 27, 28, 29, 20, 21, 23, 30):
        return {ra}
    if op in (37, 39, 45, 49, 51, 53, 55):
        return {ra}
    if op in (36, 38, 44, 48, 50, 52, 54, 2, 3, 10, 11, 4, 59, 63):
        return set()
    if op == 58:
        return {rt, ra} if (word & 3) == 1 else {rt}
    if op == 62:
        return {ra} if (word & 3) == 1 else set()
    if op in (16, 18):
        return volatile if word & 1 else set()
    if op == 17:
        return volatile
    if op == 19:
        xo = (word >> 1) & 0x3ff
        if xo in (16, 528):
            return volatile if word & 1 else set()
        return set()
    if op == 31:
        xo = (word >> 1) & 0x3ff
        logical = {28, 60, 444, 412, 316, 476, 124, 284, 24, 536, 792, 824,
                   27, 539, 794, 26, 58, 954, 922, 986}
        if xo in logical or (xo >> 1) == 413:
            return {ra}
        no_gpr = {151, 215, 407, 149, 727, 663, 231, 487, 983, 662, 918, 214, 150,
                  0, 32, 278, 246, 1014, 86, 54, 598, 854, 982, 467, 144, 146, 178,
                  4, 68, 103, 359, 7, 39, 71, 6, 38, 599, 535, 167, 199, 135, 455}
        if xo in no_gpr:
            return set()
        if xo in (183, 247, 439, 181, 631, 567, 695):
            return {ra}
        if xo in (55, 119, 311, 375, 53, 373):
            return {rt, ra}
        return {rt}
    return None


def classify_insn(word, ds):
    """(kind, width) of the instruction an access relocation sits on, or None."""
    op = word >> 26
    if ds:
        return DS_FORMS.get((op, word & 3))
    return D_FORMS.get(op)


def disp_of(word, ds):
    return sext16(word & 0xfffc) if ds else sext16(word)


# ---------------------------------------------------------------------------
# readelf-backed object model

class Section:
    def __init__(self, nr, rec):
        self.nr = nr
        self.name = rec['name']
        self.type = rec['type']
        self.size = rec['size']
        self.es = rec['es']
        self.addr = rec.get('addr', 0)
        self.off = rec.get('off', 0)
        self.flags = rec.get('flags', '')
        self.info = rec.get('info', 0)


class ObjectFile:
    """One input object or archive member, parsed through readelf."""

    def __init__(self, ident, data, s_text, sym_text, r_text):
        self.ident = ident
        self.data = data
        secs, toc_nr, rela, errs = _parse.parse_sections_pass(s_text, ident)
        if errs:
            raise InputError('; '.join(errs))
        if any('flags' not in rec for rec in secs.values()):
            raise InputError(f"{ident}: section header rows without flag/link/info columns")
        self.sections = {nr: Section(nr, rec) for nr, rec in secs.items()}
        syms, errs = _parse.parse_symbols_pass(sym_text, ident, secs)
        if errs:
            raise InputError('; '.join(errs))
        self.symbols = syms
        relocs, errs = _parse.parse_relocations_pass(r_text, ident, rela)
        if errs:
            raise InputError('; '.join(errs))
        # Relocations by the section they apply to.
        self.relocs = {}
        for rname, rows in relocs.items():
            target = self.sections[rela[rname]['nr']].info
            if target not in self.sections:
                raise InputError(f"{ident}: {rname} applies to missing section {target}")
            self.relocs.setdefault(target, []).extend(rows)
        names = {}
        for s in self.sections.values():
            names.setdefault(s.name, []).append(s.nr)
        self.by_name = names
        for s in self.sections.values():
            if s.type != 'NOBITS' and s.size and s.off + s.size > len(data):
                raise InputError(f"{ident}: section {s.name} lies outside the file")

    def bytes_of(self, sec):
        if sec.type == 'NOBITS':
            return None
        return self.data[sec.off:sec.off + sec.size]


def run_readelf(readelf, path):
    out = []
    for flag in ('-S', '-s', '-r'):
        res = subprocess.run([readelf, flag, '--wide', path], capture_output=True, text=True, check=False)
        if res.returncode != 0:
            raise InputError(f"readelf {flag} failed on {path} (rc={res.returncode}): {res.stderr.strip()}")
        out.append(res.stdout)
    return out


def load_object(readelf, path, ident):
    kind, _, err = _parse.validate_file_format(path)
    if err or kind != 'ELF':
        raise InputError(f"{ident}: not a relocatable PPU object ({err or kind})")
    s_text, sym_text, r_text = run_readelf(readelf, path)
    with open(path, 'rb') as f:
        data = f.read()
    return ObjectFile(ident, data, s_text, sym_text, r_text)


def load_archive_members(readelf, path, archive_key, wanted):
    """
    Parse the members named in `wanted` (a set).  Returns [(ObjectFile, reason)]:
    reason is None, or why the member's sites must be refused (two members
    share its name, so neither the trace nor the map tells them apart).
    """
    payloads, err = _parse.read_ar_member_payloads(path)
    if err:
        raise InputError(f"{archive_key}: {err}")
    s_text, sym_text, r_text = run_readelf(readelf, path)
    mem_S = _parse.split_readelf_by_member(s_text, path, is_archive=True)
    mem_s = _parse.split_readelf_by_member(sym_text, path, is_archive=True)
    mem_r = _parse.split_readelf_by_member(r_text, path, is_archive=True)
    names = [n for n, _ in payloads]
    if [n for n, _ in mem_S] != names or [n for n, _ in mem_s] != names or [n for n, _ in mem_r] != names:
        raise InputError(f"{archive_key}: readelf member inventory does not match the archive")
    out = []
    for name in sorted(wanted):
        idx = [i for i, n in enumerate(names) if n == name]
        if not idx:
            raise InputError(f"{archive_key}: the trace loads member {name}, which the archive lacks")
        why = None
        if len(idx) > 1:
            why = f"archive holds {len(idx)} members named {name}; the trace and the map cannot tell them apart"
        for k, i in enumerate(idx):
            ident = f"{archive_key}({name})" + (f"#{k + 1}" if why else '')
            out.append((ObjectFile(ident, payloads[i][1], mem_S[i][1], mem_s[i][1], mem_r[i][1]), why))
    return out


# ---------------------------------------------------------------------------
# Trace and map

def parse_trace(text):
    """
    ld -t -t output -> (files, members).  files: every file ld opened, in
    first-seen order (objects, archives and scripts alike; ld reopens an
    archive for each search of a group, so repeats are folded); members:
    {archive_key: [member, ...]} for the archive members it loaded.
    """
    files, members = [], {}
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        m = re.match(r'^\((.+)\)([^()]+)$', line)
        if m:
            members.setdefault(norm_name(m.group(1)), []).append(m.group(2))
            continue
        if line.startswith((' ', '\t')):
            raise InputError(f"unrecognised trace line: {line}")
        key = norm_name(line)
        if key not in files:
            files.append(key)
    return files, members


def file_kind(path):
    with open(path, 'rb') as f:
        head = f.read(20)
    if head.startswith(b'!<arch>\n'):
        return 'archive'
    if head.startswith(b'\x7fELF'):
        return 'object'
    return 'script'


def parse_map(text):
    """
    GNU ld map -> (included {(archive_key, member)}, discarded [(file_key, sec)],
    placed [(file_key, sec, addr, size)]).
    """
    lines = text.splitlines()
    heads = {}
    for i, l in enumerate(lines):
        if l.strip() == 'There are no discarded input sections':
            l = 'Discarded input sections'
        for h in ('Archive member included to satisfy reference by file (symbol)',
                  'Discarded input sections', 'Memory Configuration', 'Linker script and memory map'):
            if l.strip() == h:
                if h in heads:
                    raise InputError(f"map repeats the heading '{h}'")
                heads[h] = i
    for h in ('Memory Configuration', 'Linker script and memory map'):
        if h not in heads:
            raise InputError(f"map lacks the heading '{h}'")
    # With nothing discarded ld prints 'There are no discarded input sections'.
    di = heads.get('Discarded input sections')
    ai = heads.get('Archive member included to satisfy reference by file (symbol)')
    if di is not None and not heads['Memory Configuration'] > di:
        raise InputError("map headings are out of order")
    if ai is not None and not (di if di is not None else heads['Memory Configuration']) > ai:
        raise InputError("map headings are out of order")
    included = set()
    if ai is not None:
        for l in lines[ai + 1:di if di is not None else heads['Memory Configuration']]:
            if not l.strip() or l.startswith(' '):
                continue
            m = re.match(r'^(.*?\))(?:\s{2,}.*)?$', l)
            if not m:
                raise InputError(f"unrecognised archive-member line in map: {l}")
            arch, mem = split_member(m.group(1))
            if mem is None:
                raise InputError(f"unrecognised archive-member line in map: {l}")
            included.add((norm_name(arch), mem))

    def contributions(block, allow_output_headers):
        out = []
        pending = None
        for l in block:
            if pending is not None:
                m = re.match(r'^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(?:\s+(.*))?$', l)
                indented, name = pending
                pending = None
                if m:
                    if m.group(3) and indented:
                        out.append((name, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
                    elif indented:
                        raise InputError(f"map: input section '{name}' without a file")
                    continue
                if indented:
                    raise InputError(f"map: input section name '{name}' not followed by its placement: {l}")
                # An empty output section prints its name alone; read this line afresh.
            if not l.strip():
                continue
            m = re.match(r'^ (\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+)$', l)
            if m and not m.group(1).startswith('*'):
                out.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()))
                continue
            m = re.match(r'^( ?)([^\s*][^\s]*)$', l)
            if m and (m.group(1) or allow_output_headers) and not l.startswith(('LOAD ', 'OUTPUT(')):
                pending = (bool(m.group(1)), m.group(2))
                continue
            if re.match(r'^ [^\s*]', l) and not l.startswith((' load address', ' COMMON')):
                raise InputError(f"unrecognised input-section line in map: {l}")
        if pending is not None:
            raise InputError(f"map ends after section name '{pending[1]}'")
        return out

    disc = contributions(lines[di + 1:heads['Memory Configuration']], False) if di is not None else []
    placed = contributions(lines[heads['Linker script and memory map'] + 1:], True)
    discarded = [(norm_name(f), name) for name, _, _, f in disc]
    placed = [(norm_name(f), name, addr, size) for name, addr, size, f in placed]
    return included, discarded, placed


# ---------------------------------------------------------------------------
# Linked ELF

class LinkedElf:
    def __init__(self, readelf, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        if len(d) < 64 or d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 2:
            raise InputError("ELF: not a big-endian ELF64 file")
        if be(d, 16, 2) != 2 or be(d, 18, 2) != 21:
            raise InputError("ELF: not an EM_PPC64 executable (ET_EXEC)")
        s_text, sym_text, _ = run_readelf(readelf, path)
        secs, _, _, errs = _parse.parse_sections_pass(s_text, 'ELF')
        if errs:
            raise InputError('; '.join(errs))
        self.sections = [Section(nr, rec) for nr, rec in secs.items()]
        if not any(s.type == 'SYMTAB' for s in self.sections):
            raise InputError("ELF: no .symtab (stripped); symbols are needed to resolve globals")
        syms, errs = _parse.parse_symbols_pass(sym_text, 'ELF', secs)
        if errs:
            raise InputError('; '.join(errs))
        self.globals = {}
        tocs = []
        for s in syms.values():
            if s['name'] == '.TOC.' and s['ndx'] != 'UND':
                tocs.append(s['val'])
            if s['bind'] in ('GLOBAL', 'WEAK') and s['ndx'] != 'UND' and s['name']:
                self.globals.setdefault(s['name'], []).append(s)
        if len(tocs) != 1:
            raise InputError(f"ELF: expected one .TOC. definition, found {len(tocs)}")
        self.toc = tocs[0]
        for s in self.sections:
            if s.name in ('.stub', '.glink') and s.size:
                raise InputError(f"ELF: linker stub section {s.name} has contents (multi-TOC or PLT stubs are outside v1)")
        opd = [s for s in self.sections if s.name == '.opd']
        self.opd_count = 0
        for s in opd:
            body = self.data[s.off:s.off + s.size]
            if s.size % 8:
                raise InputError("ELF: .opd size is not a whole number of compact descriptors")
            for i in range(0, s.size, 8):
                code, toc = be(body, i, 4), be(body, i + 4, 4)
                if code == 0 and toc == 0:
                    continue
                self.opd_count += 1
                if toc != (self.toc & 0xffffffff):
                    raise InputError(f"ELF: descriptor at 0x{s.addr + i:x} carries TOC 0x{toc:x}, not .TOC. 0x{self.toc:x} (multiple TOCs are outside v1)")

    def section_at(self, addr, width):
        """The file-backed allocated section holding [addr, addr+width), or None."""
        for s in self.sections:
            if s.type == 'PROGBITS' and 'A' in s.flags and s.addr <= addr and addr + width <= s.addr + s.size:
                return s
        return None

    def read(self, addr, width):
        s = self.section_at(addr, width)
        if s is None:
            return None
        o = s.off + addr - s.addr
        return self.data[o:o + width]

    def global_value(self, name):
        hits = self.globals.get(name, [])
        if len(hits) != 1:
            return None, f"global {name} has {len(hits)} definitions in the ELF"
        return hits[0]['val'], None


# ---------------------------------------------------------------------------
# The check

class Checker:
    def __init__(self, elf, placement, discarded):
        self.elf = elf
        self.placement = placement      # (ident, secname) -> [(addr, size)]
        self.discarded = discarded      # set of (ident, secname)
        self.anchor_cache = {}
        self.sites = {}                 # (ident, secnr, index) -> disposition
        self.details = []
        self.counts = {'accesses': 0}

    # -- placement ---------------------------------------------------------
    def place(self, obj, sec):
        """(addr, None) | (None, 'discarded') | (None, reason)."""
        if len(obj.by_name.get(sec.name, [])) > 1:
            return None, f"{obj.ident} has {len(obj.by_name[sec.name])} sections named {sec.name}"
        hits = self.placement.get((obj.ident, sec.name), [])
        if len(hits) > 1:
            return None, f"map places {sec.name} of {obj.ident} {len(hits)} times"
        if hits:
            addr, size = hits[0]
            if size != sec.size:
                return None, f"map size 0x{size:x} of {sec.name} in {obj.ident} differs from the object's 0x{sec.size:x}"
            return addr, None
        if (obj.ident, sec.name) in self.discarded:
            return None, 'discarded'
        return None, f"map neither places nor discards {sec.name} of {obj.ident}"

    def anchored(self, obj, sec):
        """Placement plus a byte comparison of the contribution's unrelocated bytes."""
        key = (obj.ident, sec.nr)
        if key in self.anchor_cache:
            return self.anchor_cache[key]
        addr, why = self.place(obj, sec)
        result = (addr, why)
        if addr is not None and sec.type != 'NOBITS' and sec.size:
            out = self.elf.read(addr, sec.size)
            src = obj.bytes_of(sec)
            if out is None:
                result = (None, f"{sec.name} of {obj.ident} maps outside the ELF's file-backed sections")
            else:
                covered = bytearray(sec.size)
                calls = set()
                exec_ = 'X' in sec.flags
                for r in obj.relocs.get(sec.nr, []):
                    rt = r['rtype']
                    wide = rt in ('R_PPC64_ADDR64', 'R_PPC64_UADDR64', 'R_PPC64_TOC', 'R_PPC64_REL64')
                    if exec_:
                        # The linker may rewrite the whole instruction a relocation sits on.
                        start, width = r['offset'] & ~3, 8 if wide else 4
                    elif wide:
                        start, width = r['offset'], 8
                    elif rt in ('R_PPC64_ADDR32', 'R_PPC64_UADDR32', 'R_PPC64_REL32'):
                        start, width = r['offset'], 4
                    elif rt in ('R_PPC64_ADDR16', 'R_PPC64_UADDR16'):
                        start, width = r['offset'], 2
                    else:
                        start, width = r['offset'], 8
                    for b in range(start, min(start + width, sec.size)):
                        covered[b] = 1
                    if rt.startswith(('R_PPC64_REL24', 'R_PPC64_REL14')):
                        calls.add(r['offset'] & ~3)
                for i in range(0, sec.size):
                    if covered[i] or src[i] == out[i]:
                        continue
                    w = i & ~3
                    if exec_ and w - 4 in calls and be(src, w, 4) == NOP and be(out, w, 4) in TOC_RESTORES:
                        continue
                    result = (None, f"{sec.name} of {obj.ident}: byte +0x{i:x} differs from the ELF at 0x{addr + i:x} (stale or shifted placement)")
                    break
        self.anchor_cache[key] = result
        return result

    # -- symbol resolution ---------------------------------------------------
    def resolve(self, obj, sym_idx, addend):
        """
        Value of symbol+addend as the link must have made it:
          ('toc', offset)          inside this object's .toc
          ('addr', value)
          ('content', obj, sec, offset)   inside a merged section
        or ('refuse', reason).
        """
        sym = obj.symbols.get(sym_idx)
        if sym is None:
            return ('refuse', f"{obj.ident}: relocation names missing symbol {sym_idx}")
        if sym['type'] == 'TLS':
            return ('refuse', f"{obj.ident}: TLS symbol {sym['name']}")
        ndx = sym['ndx']
        local = sym['bind'] == 'LOCAL'
        if ndx == 'ABS' and local:
            return ('addr', sym['val'] + addend)
        if ndx.isdigit() and local:
            sec = obj.sections[int(ndx)]
            off = sym['val'] + addend
            if sec.name == '.toc':
                return ('toc', off)
            if sec.name.startswith('.toc'):
                return ('refuse', f"{obj.ident}: target in {sec.name}, a TOC section v1 does not model")
            if 'M' in sec.flags:
                return ('content', obj, sec, off)
            if sec.name == '.opd':
                return self.resolve_descriptor(obj, sec, off)
            addr, why = self.anchored(obj, sec)
            if addr is None:
                return ('refuse', f"target section: {why}")
            return ('addr', addr + off)
        # Global, weak, common or undefined: the ELF's symbol table decides.
        if ndx == 'UND' and sym['bind'] == 'WEAK' and sym['name'] not in self.elf.globals:
            return ('addr', addend)
        val, why = self.elf.global_value(sym['name'])
        if why:
            return ('refuse', why)
        if ndx.isdigit():
            sec = obj.sections[int(ndx)]
            if sec.name not in ('.opd',) and 'M' not in sec.flags and not sec.name.startswith('.toc'):
                addr, pwhy = self.anchored(obj, sec)
                if addr is not None and addr + sym['val'] != val:
                    return ('refuse', f"{sym['name']}: the ELF says 0x{val:x}, the placement of {sec.name} says 0x{addr + sym['val']:x}")
        return ('addr', val + addend)

    def resolve_descriptor(self, obj, sec, off):
        """A local function descriptor: anchored through the code address it holds."""
        addr, why = self.place(obj, sec)
        if addr is None:
            return ('refuse', f"descriptor section: {why}")
        rel = [r for r in obj.relocs.get(sec.nr, []) if r['offset'] == off]
        if len(rel) != 1 or rel[0]['rtype'] != 'R_PPC64_ADDR32':
            return ('refuse', f"{obj.ident}: no compact descriptor at .opd+0x{off:x}")
        tgt = self.resolve(obj, rel[0]['info'] >> 32, rel[0]['addend'])
        if tgt[0] != 'addr':
            return ('refuse', f"{obj.ident}: descriptor .opd+0x{off:x} code target unresolved")
        out = self.elf.read(addr + off, 4)
        if out is None or be(out, 0, 4) != (tgt[1] & 0xffffffff):
            return ('refuse', f"{obj.ident}: descriptor .opd+0x{off:x} does not hold its code address in the ELF")
        return ('addr', addr + off)

    # -- value constraints -----------------------------------------------------
    def content_matches(self, target, addr):
        """A merged-section target: does the ELF hold the same datum at addr?"""
        _, obj, sec, off = target
        src = obj.bytes_of(sec)
        if src is None or off < 0 or off >= sec.size:
            return False, f"merged target +0x{off:x} outside {sec.name}"
        es = sec.es or 1
        if 'S' in sec.flags:
            end = off
            while end + es <= sec.size and src[end:end + es] != bytes(es):
                end += es
            want = src[off:end + es]
        else:
            if off % es:
                return False, f"merged constant offset 0x{off:x} not a multiple of {es}"
            want = src[off:off + es]
        got = self.elf.read(addr, len(want))
        if got is None:
            return False, f"0x{addr:x} is not in a file-backed section"
        if got != want:
            return False, f"0x{addr:x} does not hold the datum {sec.name}+0x{off:x} names"
        return True, None

    def value_ok(self, target, value, width):
        """Check an address-sized value against a resolved target."""
        mask = (1 << (8 * width)) - 1
        if target[0] == 'addr':
            if (target[1] & mask) != (value & mask):
                return False, f"value 0x{value & mask:x}, expected 0x{target[1] & mask:x}"
            return True, None
        if target[0] == 'content':
            return self.content_matches(target, value & mask)
        return False, f"unsupported target {target[0]}"

    def toc_fields(self, obj):
        """The object's .toc as literal bytes plus (start, width, target) fields."""
        nrs = obj.by_name.get('.toc', [])
        if len(nrs) != 1:
            return None, None, f"{obj.ident} has {len(nrs)} .toc sections"
        sec = obj.sections[nrs[0]]
        fields = []
        for r in sorted(obj.relocs.get(sec.nr, []), key=lambda r: r['offset']):
            rt = r['rtype']
            if rt == 'R_PPC64_ADDR64':
                fields.append((r['offset'], 8, self.resolve(obj, r['info'] >> 32, r['addend'])))
            elif rt == 'R_PPC64_ADDR32':
                t = self.resolve(obj, r['info'] >> 32, r['addend'])
                if t[0] == 'addr' and not 0 <= t[1] < (1 << 32):
                    t = ('refuse', f"ADDR32 entry value 0x{t[1]:x} does not fit")
                fields.append((r['offset'], 4, t))
            elif rt == 'R_PPC64_TOC':
                fields.append((r['offset'], 8, ('addr', self.elf.toc)))
            else:
                fields.append((r['offset'], 8, ('refuse', f".toc entry relocation {rt}")))
        return sec, fields, None

    def check_toc_read(self, obj, toc_off, width, ea):
        """A load of `width` bytes of this object's .toc at toc_off, read at ea in the ELF."""
        sec, fields, why = self.toc_fields(obj)
        if why:
            return 'refused', why
        if toc_off < 0 or toc_off + width > sec.size:
            return 'refused', f".toc+0x{toc_off:x} width {width} outside the object's .toc (0x{sec.size:x})"
        src = obj.bytes_of(sec)
        got = self.elf.read(ea, width)
        if got is None:
            return 'failed', f"read of {width} bytes at 0x{ea:x} is not inside a file-backed section"
        lo, hi = toc_off, toc_off + width
        covered = set()
        for fs, fw, tgt in fields:
            if fs + fw <= lo or fs >= hi:
                continue
            for b in range(fs, fs + fw):
                covered.add(b)
            if tgt[0] == 'refuse':
                return 'refused', tgt[1]
            whole = self.elf.read(ea - (toc_off - fs), fw)
            if whole is None:
                return 'failed', f"entry field at 0x{ea - (toc_off - fs):x} is not inside a file-backed section"
            ok, msg = self.value_ok(tgt, be(whole, 0, fw), fw)
            if not ok:
                return 'failed', (f"{width}-byte read of .toc+0x{toc_off:x} lands at 0x{ea:x}, where the "
                                  f"{fw}-byte entry .toc+0x{fs:x} holds the wrong {msg}")
        for b in range(lo, hi):
            if b not in covered and src[b] != got[b - lo]:
                return 'failed', f"literal .toc+0x{b:x} differs at 0x{ea + b - lo:x}"
        return 'verified', None

    def check_toc_addi(self, obj, toc_off, value):
        """An ld of a .toc entry that the linker turned into addi of the entry's value."""
        sec, fields, why = self.toc_fields(obj)
        if why:
            return 'refused', why
        hit = [f for f in fields if f[0] == toc_off and f[1] == 8]
        if len(hit) != 1:
            return 'refused', f"addi rewrite of .toc+0x{toc_off:x}, which is not one 8-byte entry"
        tgt = hit[0][2]
        if tgt[0] == 'refuse':
            return 'refused', tgt[1]
        ok, msg = self.value_ok(tgt, value, 8)
        return ('verified', None) if ok else ('failed', f"addi rewrite of .toc+0x{toc_off:x}: {msg}")

    # -- per-section access walk ---------------------------------------------
    def check_section(self, obj, sec, sites):
        """sites: [(index, reloc)] of the relevant relocations applied to sec."""
        addr, why = self.anchored(obj, sec)
        if addr is None and why == 'discarded':
            for i, _ in sites:
                self.dispose(obj, sec, i, 'discarded', None)
            return
        if addr is None:
            for i, r in sites:
                self.dispose(obj, sec, i, 'refused', why, r)
            return
        src = obj.bytes_of(sec)
        if src is None:
            for i, r in sites:
                self.dispose(obj, sec, i, 'refused', f"access relocation in NOBITS {sec.name}", r)
            return
        ha = []
        rest = []
        for i, r in sites:
            role = SUPPORTED_ACCESS.get(r['rtype'])
            if role is None:
                self.dispose(obj, sec, i, 'refused', f"relocation type {r['rtype']} is outside v1", r)
                continue
            if (r['offset'] & 3) != 2 or r['offset'] + 2 > sec.size:
                self.dispose(obj, sec, i, 'refused', f"{r['rtype']} at +0x{r['offset']:x} is not on an instruction's low halfword", r)
                continue
            (ha if role == 'ha' else rest).append((i, r, role))
        ha_info = {}
        for i, r, _ in ha:
            w = be(src, r['offset'] - 2, 4)
            if w >> 26 != 15 or (w >> 16) & 31 != 2:
                self.dispose(obj, sec, i, 'refused', f"TOC16_HA at +0x{r['offset']:x} is not on addis rX,r2", r)
                continue
            ha_info[i] = {'reloc': r, 'reg': (w >> 21) & 31, 'used': []}
        for i, r, role in rest:
            self.check_access(obj, sec, src, addr, i, r, role, ha_info)
        for i, h in ha_info.items():
            if not h['used']:
                self.dispose(obj, sec, i, 'refused', f"TOC16_HA at +0x{h['reloc']['offset']:x} has no paired access", h['reloc'])
                continue
            states = [s for s, _ in h['used']]
            worst = 'failed' if 'failed' in states else 'refused' if 'refused' in states else 'verified'
            # The paired access already reported why; the addis shares its outcome.
            self.dispose(obj, sec, i, worst, None, h['reloc'])

    def check_access(self, obj, sec, src, base, i, r, role, ha_info):
        self.counts['accesses'] += 1
        ds = r['rtype'] in DS_RELOCS
        insn_off = r['offset'] - 2
        w = be(src, insn_off, 4)
        form = classify_insn(w, ds)
        where = f"{obj.ident} {sec.name}+0x{insn_off:x}"

        def done(state, msg, pair=None):
            self.dispose(obj, sec, i, state, msg, r)
            if pair is not None:
                pair['used'].append((state, msg))

        if form is None:
            return done('refused', f"{where}: {r['rtype']} on an instruction form v1 does not model (0x{w:08x})")
        kind, width = form
        ra = (w >> 16) & 31
        rt = (w >> 21) & 31
        pair = None
        if role == 'lo':
            if ra == 2:
                return done('refused', f"{where}: {r['rtype']} based on r2 without its TOC16_HA")
            key = (r['info'] >> 32, r['addend'])
            cands = [(j, h) for j, h in ha_info.items()
                     if (h['reloc']['info'] >> 32, h['reloc']['addend']) == key
                     and h['reg'] == ra and h['reloc']['offset'] < r['offset']]
            if not cands:
                return done('refused', f"{where}: no preceding TOC16_HA of the same target sets r{ra}")
            j, pair = max(cands, key=lambda c: c[1]['reloc']['offset'])
            for off in range(pair['reloc']['offset'] + 2, insn_off, 4):
                writes = gpr_writes(be(src, off, 4))
                if writes is None or ra in writes:
                    return done('refused', f"{where}: r{ra} may change at +0x{off:x} between its addis and this use", pair)
        elif ra != 2:
            return done('refused', f"{where}: {r['rtype']} not based on r2")
        target = self.resolve(obj, r['info'] >> 32, r['addend'])
        if target[0] == 'refuse':
            return done('refused', f"{where}: {target[1]}", pair)

        # Decode the ELF's instructions.
        out_w = self.elf.read(base + insn_off, 4)
        if out_w is None:
            return done('failed', f"{where}: 0x{base + insn_off:x} is not in a file-backed section", pair)
        ow = be(out_w, 0, 4)
        hi = 0
        expect_ra = 2
        if pair is not None:
            hw_b = self.elf.read(base + pair['reloc']['offset'] - 2, 4)
            hw = be(hw_b, 0, 4) if hw_b else None
            if hw == NOP:
                expect_ra = 2
            elif hw is not None and hw >> 26 == 15 and (hw >> 16) & 31 == 2 and (hw >> 21) & 31 == ra:
                hi = sext16(hw)
                expect_ra = ra
            else:
                return done('refused', f"{where}: its addis became 0x{(hw or 0):08x}, a rewrite v1 does not model", pair)
        o_op, o_rt, o_ra = ow >> 26, (ow >> 21) & 31, (ow >> 16) & 31
        rewritten_addi = False
        if o_op == (w >> 26) and o_rt == rt and (not ds or (ow & 3) == (w & 3)):
            pass
        elif ds and kind == 'load' and width == 8 and o_op == 14 and o_rt == rt:
            rewritten_addi = True
        else:
            return done('refused', f"{where}: the ELF has 0x{ow:08x} for 0x{w:08x}, a rewrite v1 does not model", pair)
        if o_ra != expect_ra:
            if pair is not None and expect_ra == 2 and o_ra == ra:
                return done('failed', f"{where}: its addis is gone but the access still uses r{ra}", pair)
            return done('refused', f"{where}: the ELF bases the access on r{o_ra}, expected r{expect_ra}", pair)
        disp = sext16(ow) if rewritten_addi else disp_of(ow, ds and not rewritten_addi)
        ea = (self.elf.toc + (hi << 16) + disp) & 0xffffffffffffffff

        if target[0] == 'toc':
            if rewritten_addi:
                state, msg = self.check_toc_addi(obj, target[1], ea)
            elif kind == 'load':
                state, msg = self.check_toc_read(obj, target[1], width, ea)
            else:
                state, msg = 'refused', f"{kind} on a .toc entry"
        else:
            if rewritten_addi:
                state, msg = 'refused', "addi rewrite of a direct (non-.toc) load"
            elif target[0] == 'content' and kind == 'load':
                _, tobj, tsec, toff = target
                want = tobj.bytes_of(tsec)[toff:toff + width] if tobj.bytes_of(tsec) else None
                got = self.elf.read(ea, width)
                if want is None or len(want) != width:
                    state, msg = 'refused', "merged-section load outside its section"
                elif got != want:
                    state, msg = 'failed', f"load at 0x{ea:x} does not read {tsec.name}+0x{toff:x}"
                else:
                    state, msg = 'verified', None
            else:
                ok, m = self.value_ok(target, ea, 8)
                state, msg = ('verified', None) if ok else ('failed', f"address 0x{ea:x}: {m}")
        if msg:
            msg = f"{where}: {msg}"
        return done(state, msg, pair)

    def dispose(self, obj, sec, i, state, msg, r=None):
        key = (obj.ident, sec.nr, i)
        if key in self.sites:
            raise RuntimeError(f"relocation site {key} disposed twice")
        self.sites[key] = state
        if msg and state in ('failed', 'refused'):
            self.details.append((state, msg))


def relevant(rtype):
    return 'TOC16' in rtype or rtype.startswith('R_PPC64_GOT')


# ---------------------------------------------------------------------------
# Commands

def cmd_capture(args):
    out = os.path.abspath(args.out)
    if os.path.exists(out) and os.listdir(out):
        print(f"capture: {args.out} exists and is not empty; use a fresh directory", file=sys.stderr)
        return 2
    argv = list(args.command)
    if argv and argv[0] == '--':
        argv = argv[1:]
    if not argv:
        print("capture: no link command", file=sys.stderr)
        return 2
    elf = None
    for k, a in enumerate(argv):
        if a == '-o' and k + 1 < len(argv):
            elf = argv[k + 1]
        elif a.startswith('-o') and len(a) > 2:
            elf = a[2:]
    if elf is None:
        print("capture: the link command has no -o", file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)
    cwd = os.getcwd()
    map_path = os.path.join(out, 'link.map')
    full = argv + [f'-Wl,-Map,{map_path}', '-Wl,-t', '-Wl,-t']
    res = subprocess.run(full, capture_output=True, text=True, check=False)
    with open(os.path.join(out, 'link.trace'), 'w', newline='') as f:
        f.write(res.stdout)
    with open(os.path.join(out, 'link.stderr'), 'w', newline='') as f:
        f.write(res.stderr)
    if res.returncode != 0:
        print(f"capture: link failed (rc={res.returncode}); see {out}/link.stderr", file=sys.stderr)
        return 2

    def rel(p):
        a = os.path.abspath(p)
        try:
            r = os.path.relpath(a, out)
        except ValueError:
            r = '..'
        return (a if r.startswith('..') else r).replace(chr(92), '/')

    try:
        files, members = parse_trace(res.stdout)
    except InputError as ex:
        print(f"capture: {ex}", file=sys.stderr)
        return 2
    inputs = []
    for key in files:
        path = resolve_path(key, cwd)
        inputs.append({'key': key, 'file': rel(path), 'kind': file_kind(path), 'sha256': sha256_file(path)})
    elf_path = elf if os.path.isabs(elf) else os.path.join(cwd, elf)
    manifest = {
        'format': MANIFEST_FORMAT,
        'cwd': rel(cwd),
        'argv': full,
        'elf': {'file': rel(elf_path), 'sha256': sha256_file(elf_path)},
        'map': {'file': 'link.map', 'sha256': sha256_file(map_path)},
        'trace': {'file': 'link.trace', 'sha256': sha256_file(os.path.join(out, 'link.trace'))},
        'inputs': inputs,
    }
    with open(os.path.join(out, 'manifest.json'), 'w', newline='') as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write('\n')
    print(f"capture: {len(inputs)} input files, {sum(len(v) for v in members.values())} archive members; "
          f"manifest {rel(os.path.join(out, 'manifest.json'))}")
    return 0


def run_check(manifest_path, readelf):
    """Returns (status, counts, details, notes); raises InputError for bad input."""
    mdir = os.path.dirname(os.path.abspath(manifest_path))
    with open(manifest_path, 'r') as f:
        man = json.load(f)
    if man.get('format') != MANIFEST_FORMAT:
        raise InputError(f"manifest format {man.get('format')!r}, expected {MANIFEST_FORMAT}")

    def at(rel):
        if re.match(r'^[A-Za-z]:/', rel) or rel.startswith('/'):
            return rel
        return os.path.join(mdir, rel)

    for k in ('elf', 'map', 'trace'):
        rec = man.get(k) or {}
        p = at(rec.get('file', ''))
        if not rec.get('file') or not os.path.isfile(p):
            raise InputError(f"manifest {k} {rec.get('file')} is missing")
        if sha256_file(p) != rec.get('sha256'):
            raise InputError(f"manifest {k} {rec.get('file')} does not match its pinned SHA-256")
    files = {}
    for rec in man.get('inputs', []):
        p = at(rec['file'])
        if not os.path.isfile(p):
            raise InputError(f"input {rec['key']} ({rec['file']}) is missing")
        if sha256_file(p) != rec['sha256']:
            raise InputError(f"input {rec['key']} ({rec['file']}) does not match its pinned SHA-256")
        if rec['key'] in files:
            raise InputError(f"manifest lists input {rec['key']} twice")
        files[rec['key']] = p
    with open(at(man['trace']['file']), 'r', newline='') as f:
        trace_files, members = parse_trace(f.read())
    with open(at(man['map']['file']), 'r', newline='') as f:
        included, discarded, placed = parse_map(f.read())

    if set(trace_files) != set(files):
        raise InputError("manifest inputs and the pinned trace name different files")
    kinds = {k: file_kind(p) for k, p in files.items()}
    objects = [k for k in trace_files if kinds[k] == 'object']
    trace_members = set()
    for a, ms in members.items():
        if kinds.get(a) != 'archive':
            raise InputError(f"trace loads members of {a}, which the trace did not open as an archive")
        if len(ms) != len(set(ms)):
            raise InputError(f"the trace loads two members named alike from {a}")
        trace_members |= {(a, m) for m in ms}
    if included - trace_members:
        raise InputError(f"map includes archive members the trace does not load: {sorted(included - trace_members)[:3]}")
    if trace_members - included:
        raise InputError(f"trace loads archive members the map does not list as included: {sorted(trace_members - included)[:3]}")

    idents = set(objects) | {f"{a}({m})" for a, m in trace_members}

    def ident_of(fkey):
        a, m = split_member(fkey)
        return f"{norm_name(a)}({m})" if m else fkey

    placement = {}
    for fkey, name, addr, size in placed:
        if fkey == 'linker stubs':
            continue
        ident = ident_of(fkey)
        if ident not in idents:
            raise InputError(f"map places {name} from {fkey}, which the trace never loaded")
        placement.setdefault((ident, name), []).append((addr, size))
    disc = set()
    for fkey, name in discarded:
        ident = ident_of(fkey)
        if ident not in idents and fkey != 'linker stubs':
            raise InputError(f"map discards {name} from {fkey}, which the trace never loaded")
        disc.add((ident, name))

    elf = LinkedElf(readelf, at(man['elf']['file']))
    checker = Checker(elf, placement, disc)
    objs = [(load_object(readelf, files[k], k), None) for k in objects]
    for a, ms in members.items():
        objs.extend(load_archive_members(readelf, files[a], a, set(ms)))
    for o, _ in objs:
        for nr, s in o.sections.items():
            if s.name == '.opd':
                for r in o.relocs.get(nr, []):
                    if r['rtype'] != 'R_PPC64_ADDR32':
                        raise InputError(f"{o.ident}: .opd relocation {r['rtype']}; v1 handles compact descriptors only")
    total_sites = 0
    for o, why in objs:
        for nr, s in sorted(o.sections.items()):
            if s.name in ('.toc', '.opd'):
                continue
            rows = [(i, r) for i, r in enumerate(o.relocs.get(nr, [])) if relevant(r['rtype'])]
            if not rows:
                continue
            total_sites += len(rows)
            if why or 'A' not in s.flags or 'X' not in s.flags:
                for i, r in rows:
                    reason = why or f"{r['rtype']} in non-code section {s.name}"
                    checker.dispose(o, s, i, 'refused', f"{o.ident}: {reason}", r)
                continue
            checker.check_section(o, s, rows)
    tally = {k: sum(1 for v in checker.sites.values() if v == k) for k in ('verified', 'failed', 'refused', 'discarded')}
    unaccounted = total_sites - sum(tally.values())
    counts = {
        'objects': len(objs),
        'relocation_sites': total_sites,
        'sites_verified': tally['verified'],
        'sites_failed': tally['failed'],
        'sites_refused': tally['refused'],
        'sites_discarded': tally['discarded'],
        'sites_unaccounted': unaccounted,
        'accesses': checker.counts['accesses'],
        'descriptors': elf.opd_count,
    }
    if tally['failed']:
        status = 'FAIL'
    elif tally['refused'] or unaccounted or tally['verified'] == 0:
        status = 'INCOMPLETE'
    else:
        status = 'PASS'
    return status, counts, checker.details, {'toc': elf.toc}


def cmd_check(args):
    readelf = _parse.find_tool('powerpc64-ps3-elf-readelf', args.readelf, ['PPU_READELF'])
    if not readelf:
        return 2
    try:
        status, counts, details, notes = run_check(args.manifest, readelf)
    except InputError as ex:
        print(f"toc-linked-check: ERROR: {ex}")
        return 2
    except (OSError, ValueError, KeyError) as ex:
        print(f"toc-linked-check: ERROR: unreadable input: {ex}")
        return 2
    print(f"toc-linked-check: .TOC. 0x{notes['toc']:x}, {counts['descriptors']} descriptors on one TOC")
    for k in ('objects', 'relocation_sites', 'sites_verified', 'sites_failed', 'sites_refused',
              'sites_discarded', 'sites_unaccounted', 'accesses'):
        print(f"  {k:<20} {counts[k]}")
    shown = details if args.verbose else details[:args.max_detail]
    for state, msg in shown:
        print(f"  {state.upper():<8} {msg}")
    if len(shown) < len(details):
        print(f"  ... {len(details) - len(shown)} more (use -v)")
    if args.report:
        with open(args.report, 'w', newline='') as f:
            json.dump({'status': status, 'counts': counts,
                       'details': [{'state': s, 'detail': m} for s, m in details]}, f, indent=1, sort_keys=True)
            f.write('\n')
    print(f"toc-linked-check: {status}")
    return 0 if status == 'PASS' else 1


def main():
    p = argparse.ArgumentParser(description="Check a linked PPU ELF's TOC accesses against its input objects.")
    sub = p.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture', help="run a link and pin its map, trace and inputs")
    c.add_argument('--out', required=True, help="fresh directory for the manifest, map and trace")
    c.add_argument('command', nargs=argparse.REMAINDER, help="-- followed by the link command")
    k = sub.add_parser('check', help="check a captured link")
    k.add_argument('manifest')
    k.add_argument('--readelf', help="readelf to use (default: the PPU readelf, then readelf on PATH)")
    k.add_argument('--report', help="also write a JSON report here")
    k.add_argument('--max-detail', type=int, default=20)
    k.add_argument('-v', '--verbose', action='store_true')
    args = p.parse_args()
    if args.cmd == 'capture':
        return cmd_capture(args)
    return cmd_check(args)


if __name__ == '__main__':
    sys.exit(main())
