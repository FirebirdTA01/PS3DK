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
  - an HA/LO pair is joined by the control flow inside the code section:
    every path back from the LO use must reach an addis of the same target
    in that register before any other write of it or any entry point, and
    the use is checked against each such addis.  Entry points are the
    section start, its symbols, addresses other relocations name in it,
    call targets and every instruction no modelled edge reaches (landing
    pads among them).  A branch with a relocation goes where it names
    (through a local .opd descriptor to its code address); one without
    must stay in its section or the section is refused.  A computed jump
    (bcctr) reaches the targets of a switch table proven from its range
    check, table address and dispatch sequence; mtctr counts only inside
    such a sequence, and a CTR-decrementing branch or a wrapping rotate
    mask in it proves nothing.  A blr is a return only when unconditional
    and LR provably holds the entry LR there: on paths with no LR write (a
    leaf), or after the ABI epilogue (mflr rX stored to the LR save slot,
    entry SP + 16, before any call; rY reloaded from the slot at a known
    SP; mtlr rY; blr), with the stack pointer followed only through
    stdu/stwu r1,-N(r1) and addi r1,r1,N and no other store through r1
    able to reach the slot.  Any other transfer through CTR or LR (an
    unproven bcctr, bctrl, a conditional or decrementing bclr, bclrl, a
    blr without that proof) makes every instruction of the section an
    entry point with unknown registers.  RA=0 in a D-form base is literal zero: such an LO never
    pairs with an addis.  Register writes are modelled for listed
    instruction forms only; any other form may write every register;
  - names come from the files' own string tables (readelf truncates long
    section names), and must extend readelf's.

Coverage limits (these shapes are refused, by design):
  - a switch dispatch that range-checks one load and indexes its table
    with a second load of the same address: two loads are never assumed
    equal, so the jump stays unproven (libgcc's uw_update_context_1);
  - no ABI-extension rule: a 32-bit range check does not bound a shift of
    the whole register, so switch (int) lowered at -O2 as cmplwi + sldi is
    refused (cmpldi + sldi, and cmplwi + rldic ...,2,30 are proven).

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

# GPR writes of the instruction forms the HA/LO search may walk through.
# Every form is listed exactly; any other encoding is a possible write of
# every register.
D_WRITES_RT = {7, 8, 12, 13, 14, 15, 32, 34, 40, 42}   # mulli subfic addic addic. addi addis lwz lbz lhz lha
D_WRITES_RT_RA = {33, 35, 41, 43}                       # lwzu lbzu lhzu lhau
D_WRITES_RA = {20, 21, 23, 24, 25, 26, 27, 28, 29,      # rlwimi rlwinm rlwnm ori oris xori xoris andi. andis.
               37, 39, 45, 49, 51, 53, 55}              # stwu stbu sthu lfsu lfdu stfsu stfdu
D_WRITES_NONE = {2, 3, 10, 11, 36, 38, 44, 47, 48, 50, 52, 54,   # tdi twi cmpli cmpi stw stb sth stmw lfs lfd stfs stfd
                 4, 59, 63}                             # vector and floating-point arithmetic
XL_WRITES_NONE = {0, 33, 129, 150, 193, 225, 257, 289, 417, 449}  # mcrf, CR logical ops, isync
X_WRITES_RA = {28, 60, 444, 412, 316, 476, 124, 284,    # and andc or orc xor nand nor eqv
               24, 536, 792, 824, 27, 539, 794, 826, 827,   # slw srw sraw srawi sld srd srad sradi
               26, 58, 954, 922, 986,                   # cntlzw cntlzd extsb extsh extsw
               183, 247, 439, 181, 695, 759, 567, 631}  # stwux stbux sthux stdux stfsux stfdux lfsux lfdux
X_WRITES_RT = {23, 87, 279, 343, 21, 341, 20, 84, 534, 790,   # lwzx lbzx lhzx lhax ldx lwax lwarx ldarx lwbrx lhbrx
               19, 83, 339, 371}                        # mfcr/mfocrf mfmsr mfspr mftb
X_WRITES_RT_RA = {55, 119, 311, 375, 53, 373}           # lwzux lbzux lhzux lhaux ldux lwaux
X_WRITES_NONE = {151, 215, 407, 149, 662, 918, 150, 214,   # stwx stbx sthx stdx stwbrx sthbrx stwcx. stdcx.
                 663, 727, 983, 535, 599, 661, 725,     # stfsx stfdx stfiwx lfsx lfdx stswx stswi
                 7, 39, 71, 103, 359, 135, 167, 199, 231, 487, 6, 38,   # vector loads and stores, lvsl lvsr
                 519, 551, 775, 807, 647, 679, 903, 935,   # lvlx lvrx lvlxl lvrxl stvlx stvrx stvlxl stvrxl
                 0, 32, 4, 68,                          # cmp cmpl tw td
                 278, 246, 1014, 86, 54, 982, 598, 854,  # dcbt dcbtst dcbz dcbf dcbst icbi sync eieio
                 342, 374, 822,                         # dst dstst dss
                 467, 144, 146, 178}                    # mtspr mtcrf mtmsr mtmsrd
XO_WRITES_RT = {266, 40, 8, 10, 136, 138, 200, 202, 232, 234, 104,   # add subf subfc addc subfe adde subfze addze subfme addme neg
                235, 233, 75, 11, 73, 9, 491, 459, 489, 457}       # mullw mulld mulhw mulhwu mulhd mulhdu divw divwu divd divdu
VOLATILE_GPRS = frozenset({0} | set(range(3, 13)))


def gpr_writes(word):
    """
    GPRs an instruction may write, for the HA/LO continuity check.
    Returns a set of register numbers, or None when the form is not one of
    the listed ones (the caller treats that as a possible write of any
    register).  A call (LK=1) may write every volatile register.
    """
    op = word >> 26
    rt = (word >> 21) & 31
    ra = (word >> 16) & 31
    if op in D_WRITES_RT:
        return {rt}
    if op in D_WRITES_RT_RA:
        return {rt, ra}
    if op in D_WRITES_RA:
        return {ra}
    if op in D_WRITES_NONE:
        return set()
    if op == 46:                                        # lmw
        return set(range(rt, 32))
    if op == 58:                                        # ld ldu lwa
        return {0: {rt}, 1: {rt, ra}, 2: {rt}}.get(word & 3)
    if op == 62:                                        # std stdu
        return {0: set(), 1: {ra}}.get(word & 3)
    if op == 30:                                        # rldicl rldicr rldic rldimi, rldcl rldcr
        if (word >> 2) & 7 in (0, 1, 2, 3) or (word >> 1) & 15 in (8, 9):
            return {ra}
        return None
    if op in (16, 18):                                  # bc, b
        return set(VOLATILE_GPRS) if word & 1 else set()
    if op == 17:                                        # sc
        return set(VOLATILE_GPRS)
    if op == 19:
        xo = (word >> 1) & 0x3ff
        if xo in (16, 528):                             # bclr, bcctr
            return set(VOLATILE_GPRS) if word & 1 else set()
        return set() if xo in XL_WRITES_NONE else None
    if op == 31:
        xo = (word >> 1) & 0x3ff
        if xo in X_WRITES_RA:
            return {ra}
        if xo in X_WRITES_RT:
            return {rt}
        if xo in X_WRITES_RT_RA:
            return {rt, ra}
        if xo in X_WRITES_NONE:
            return set()
        if xo == 597:                                   # lswi: NB bytes (0 means 32) into rt, rt+1, ... wrapping
            nb = (word >> 11) & 31 or 32
            return {(rt + k) % 32 for k in range((nb + 3) // 4)}
        if (word >> 1) & 0x1ff in XO_WRITES_RT:         # XO forms, with or without OE
            return {rt}
        return None                                     # lswx (count from XER) and every unlisted form
    return None


# The LR save slot, as offset from the entry stack pointer and width, per data
# model.  Measured in the compilers' own prologues and epilogues across the
# real-link rows (mflr rX; std rX,16(r1) before the frame is allocated; ld
# from entry SP + 16 before mtlr): ILP32 and LP64 agree.
LR_SAVE_SLOT = {'ilp32': (16, 8), 'lp64': (16, 8)}
LR_SLOT_OFF, LR_SLOT_WIDTH = LR_SAVE_SLOT['ilp32']
assert LR_SAVE_SLOT['lp64'] == (LR_SLOT_OFF, LR_SLOT_WIDTH)

# Stores: primary opcode -> width (D forms; DS forms by their low bits).
D_STORE_WIDTH = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2, 52: 4, 53: 4, 54: 8, 55: 8}
# X-form stores and other memory writers (by extended opcode); the address
# is register-indexed, so an r1 operand is an unknown overlap.
X_MEMORY_WRITES = {151, 183, 215, 247, 407, 439, 149, 181, 662, 918, 150, 214, 663, 695, 727, 759,
                   983, 661, 725, 135, 167, 199, 231, 487, 647, 679, 903, 935, 1014}


def stack_write(word):
    """
    How an instruction may write memory relative to r1: None (it does not
    write memory through r1), ('at', disp, width), ('update', disp, width)
    for a store with update, or ('unknown',) when the address or width is
    not a constant offset from r1.
    """
    op, rs, ra, rb = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    if op in D_STORE_WIDTH and ra == 1:
        return ('update' if op & 1 else 'at', sext16(word & 0xffff), D_STORE_WIDTH[op])
    if op == 62 and ra == 1:
        if word & 3 in (0, 1):
            return ('update' if word & 3 else 'at', sext16(word & 0xfffc), 8)
        return ('unknown',)
    if op == 47 and ra == 1:
        return ('at', sext16(word & 0xffff), 4 * (32 - rs))        # stmw
    if op == 31 and (word >> 1) & 0x3ff in X_MEMORY_WRITES and 1 in (ra, rb):
        return ('unknown',)
    return None


def may_write_cr(word):
    """
    Whether an instruction may write a condition register field: compares,
    record (Rc=1) forms, CR moves and logic, traps never; calls may.
    """
    op = word >> 26
    if op in (10, 11, 13, 28, 29, 4, 17, 59, 63):        # cmpli cmpi addic. andi. andis. vector sc floating point
        return True
    if op in (16, 18):
        return bool(word & 1)
    if op == 19:
        return (word >> 1) & 0x3ff not in (16, 528, 150) or bool(word & 1)
    if op in (20, 21, 23, 30, 31):
        return bool(word & 1) or (op == 31 and (word >> 1) & 0x3ff in (0, 32, 144, 512, 150, 214))
    if op in D_WRITES_RT | D_WRITES_RT_RA | D_WRITES_RA | D_WRITES_NONE | {46, 58, 62, 2, 3}:
        return False
    return True


def classify_insn(word, ds):
    """(kind, width) of the instruction an access relocation sits on, or None."""
    op = word >> 26
    if ds:
        return DS_FORMS.get((op, word & 3))
    return D_FORMS.get(op)


def disp_of(word, ds):
    return sext16(word & 0xfffc) if ds else sext16(word)


# ---------------------------------------------------------------------------
# Names from the file itself (readelf -W still truncates very long names)

def raw_names(data, what):
    """
    (section names by index, symbol names by .symtab index) read from the
    ELF's own string tables; raises InputError on a malformed header.
    """
    if len(data) < 64 or data[:4] != bytes([0x7f]) + b'ELF' or data[4] != 2 or data[5] != 2:
        raise InputError(f"{what}: not a big-endian ELF64 file")
    shoff, shentsize, shnum, shstrndx = be(data, 40, 8), be(data, 58, 2), be(data, 60, 2), be(data, 62, 2)
    if shentsize != 64 or shoff + shnum * 64 > len(data) or shstrndx >= shnum:
        raise InputError(f"{what}: section header table outside the file")

    def shdr(i):
        o = shoff + 64 * i
        return {'name': be(data, o, 4), 'type': be(data, o + 4, 4), 'off': be(data, o + 24, 8),
                'size': be(data, o + 32, 8), 'link': be(data, o + 40, 4)}

    def cstr(tab, at):
        if tab['off'] + tab['size'] > len(data) or at >= tab['size']:
            raise InputError(f"{what}: string offset outside its table")
        body = data[tab['off'] + at:tab['off'] + tab['size']]
        end = body.find(bytes(1))
        if end < 0:
            raise InputError(f"{what}: unterminated string table entry")
        return body[:end].decode('utf-8', 'replace')

    hdrs = [shdr(i) for i in range(shnum)]
    shstr = hdrs[shstrndx]
    sec_names = [cstr(shstr, h['name']) if i else '' for i, h in enumerate(hdrs)]
    sym_names = {}
    tabs = [h for h in hdrs if h['type'] == 2]
    if len(tabs) == 1:
        t = tabs[0]
        if t['link'] >= shnum or t['off'] + t['size'] > len(data):
            raise InputError(f"{what}: .symtab outside the file")
        strs = hdrs[t['link']]
        for k in range(t['size'] // 24):
            sym_names[k] = cstr(strs, be(data, t['off'] + 24 * k, 4))
    return sec_names, sym_names


def adopt_names(what, data, sections, symbols):
    """Replace readelf's names by the file's, which readelf's must prefix."""
    sec_names, sym_names = raw_names(data, what)
    if len(sec_names) != len(sections):
        raise InputError(f"{what}: readelf lists {len(sections)} sections, the file has {len(sec_names)}")
    for nr, rec in sections.items():
        if not sec_names[nr].startswith(rec.name):
            raise InputError(f"{what}: section [{nr}] is {sec_names[nr]!r} in the file but {rec.name!r} in readelf")
        rec.name = sec_names[nr]
    for idx, sym in symbols.items():
        if sym['type'] == 'SECTION':
            continue
        full = sym_names.get(idx)
        if full is None or not full.startswith(sym['name']):
            raise InputError(f"{what}: symbol [{idx}] is {full!r} in the file but {sym['name']!r} in readelf")
        sym['name'] = full


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
        adopt_names(ident, data, self.sections, self.symbols)
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
        adopt_names('ELF', self.data, {s.nr: s for s in self.sections}, syms)
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
        self.flow_cache = {}
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
          ('content', obj, sec, base, delta)   inside a merged section
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
                # ld resolves a section symbol in a merged section to the entry
                # holding st_value + addend, but any other symbol to its own
                # entry plus the addend, which may point outside that entry
                # (a string walk's .LC0 - 1).
                if sym['type'] == 'SECTION':
                    base, delta = off, 0
                else:
                    base, delta = sym['val'], addend
                if not 0 <= base < sec.size:
                    return ('refuse', f"{obj.ident}: merged-section symbol at +0x{base:x} outside {sec.name}")
                return ('content', obj, sec, base, delta)
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
    def merged_entry(self, obj, sec, base):
        """
        The bytes of the merged entry ld keeps together with source offset
        base: the string from base to its terminator, or the whole
        fixed-size constant holding base.  (start, data) or (None, reason).
        """
        src = obj.bytes_of(sec)
        if src is None:
            return None, f"merged {sec.name} has no contents"
        es = sec.es or 1
        if 'S' in sec.flags:
            end = base
            while end + es <= sec.size and src[end:end + es] != bytes(es):
                end += es
            if end + es > sec.size:
                return None, f"string at {sec.name}+0x{base:x} has no terminator"
            return base, src[base:end + es]
        start = base - base % es
        if start + es > sec.size:
            return None, f"constant at {sec.name}+0x{base:x} runs past the section"
        return start, src[start:start + es]

    def content_matches(self, target, addr):
        """
        A merged-section target: addr is where the link put symbol+addend.
        The entry the symbol names must sit at addr - addend.
        """
        _, obj, sec, base, delta = target
        start, data = self.merged_entry(obj, sec, base)
        if start is None:
            return False, data
        at = addr - delta - (base - start)
        got = self.elf.read(at, len(data))
        if got is None:
            return False, f"0x{at:x} is not in a file-backed section"
        if got != data:
            return False, f"0x{at:x} does not hold the datum {sec.name}+0x{base:x} names"
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
        ha_at = {h['reloc']['offset'] - 2: (j, h) for j, h in ha_info.items()}
        for i, r, role in rest:
            self.check_access(obj, sec, src, addr, i, r, role, ha_info, ha_at)
        for i, h in ha_info.items():
            if not h['used']:
                self.dispose(obj, sec, i, 'refused',
                             f"{obj.ident} {sec.name}+0x{h['reloc']['offset'] - 2:x}: TOC16_HA has no paired access", h['reloc'])
                continue
            states = [s for s, _ in h['used']]
            worst = 'failed' if 'failed' in states else 'refused' if 'refused' in states else 'verified'
            # The paired access already reported why; the addis shares its outcome.
            self.dispose(obj, sec, i, worst, None, h['reloc'])

    # -- control flow for HA/LO pairing ------------------------------------
    def branch_target(self, obj, sec, size, off, r):
        """
        Where a branch relocation at +off sends control: ('in', offset) for an
        instruction of this section, ('out', None) for another section, or
        ('bad', reason).
        """
        sym = obj.symbols.get(r['info'] >> 32)
        if sym is None:
            return 'bad', f"branch at +0x{off:x} names missing symbol {r['info'] >> 32}"
        if sym['ndx'].isdigit() and obj.sections[int(sym['ndx'])].name == '.opd':
            # A call through a local descriptor goes to the code address it holds.
            at = sym['val'] + r['addend']
            d = [x for x in obj.relocs.get(int(sym['ndx']), []) if x['offset'] == at]
            if len(d) != 1 or d[0]['rtype'] != 'R_PPC64_ADDR32':
                return 'bad', f"branch at +0x{off:x} names .opd+0x{at:x}, which holds no code address"
            sym = obj.symbols.get(d[0]['info'] >> 32)
            if sym is None:
                return 'bad', f"descriptor .opd+0x{at:x} names missing symbol {d[0]['info'] >> 32}"
            r = {'addend': d[0]['addend']}
        if sym['ndx'] == str(sec.nr):
            t = sym['val'] + r['addend']
            if 0 <= t < size and t % 4 == 0:
                return 'in', t
            return 'bad', f"branch at +0x{off:x} names +0x{t:x}, not an instruction of {sec.name}"
        if r['addend']:
            # symbol+addend in another section may still land in this one
            return 'bad', f"branch at +0x{off:x} names {sym['name']}{r['addend']:+#x} outside {sec.name}"
        return 'out', None

    def flow(self, obj, sec, src):
        """
        The section's intra-section control flow, for the HA/LO register
        search: predecessors of each instruction, the entry points (where
        control may arrive with unknown registers).  Entry points are the
        section start, its symbols, the addresses other relocations name in
        it, the targets of calls, and every instruction no modelled edge
        reaches (a landing pad, for one).  A branch with a relocation goes
        where the relocation names; one without stays inside the section or
        the section is refused.  A computed jump (bcctr) reaches the targets
        of a proven switch table (switch_targets), or else any instruction
        of the section, with the registers it had at the jump.
        """
        key = (obj.ident, sec.nr)
        if key in self.flow_cache:
            return self.flow_cache[key]
        size = sec.size - sec.size % 4
        rels = obj.relocs.get(sec.nr, [])
        bad = None
        branch_rel = {}
        for r in rels:
            if r['rtype'].startswith(('R_PPC64_REL24', 'R_PPC64_REL14', 'R_PPC64_ADDR24', 'R_PPC64_ADDR14')):
                k = r['offset'] & ~3
                if k in branch_rel:
                    bad = bad or f"two branch relocations on the instruction at +0x{k:x}"
                branch_rel[k] = r
        preds, indirect, calls = {}, [], set()
        returns, unknown = [], None

        def leave(why):
            nonlocal bad
            bad = bad or why

        for off in range(0, size, 4):
            w = be(src, off, 4)
            op = w >> 26
            xo = (w >> 1) & 0x3ff
            nxt = [off + 4] if off + 4 < size else []
            succ = nxt
            if op in (16, 18):
                link = w & 1
                always = op == 18 or ((w >> 21) & 0x14) == 0x14
                succ = nxt if link or not always else []
                if off in branch_rel:
                    where, t = self.branch_target(obj, sec, size, off, branch_rel[off])
                    if where == 'bad':
                        leave(t)
                    elif where == 'in':
                        if link:
                            calls.add(t)
                        else:
                            succ = succ + [t]
                elif w & 2:
                    leave(f"absolute branch at +0x{off:x} without a relocation")
                else:
                    disp = ((w & 0x3fffffc) ^ 0x2000000) - 0x2000000 if op == 18 else sext16(w & 0xfffc)
                    t = off + disp
                    if not 0 <= t < size:
                        leave(f"branch at +0x{off:x} leaves {sec.name} without a relocation")
                    elif link:
                        calls.add(t)
                    else:
                        succ = succ + [t]
            elif op == 19 and xo in (16, 528):
                always = ((w >> 21) & 0x14) == 0x14
                succ = nxt if (w & 1) or not always else []
                if xo == 16 and always and not w & 1:
                    returns.append(off)                     # blr: a return only while LR is the entry LR
                elif xo == 528 and not w & 1 and (w >> 21) & 4:
                    indirect.append(off)                    # bctr/bcctr: a switch, or unknown
                else:
                    unknown = unknown or (f"computed {'call' if w & 1 else 'jump'} through "
                                          f"{'LR' if xo == 16 else 'CTR'} at +0x{off:x}")
            for t in succ:
                preds.setdefault(t, []).append(off)
        entries = {0} | calls
        for s_ in obj.symbols.values():
            if s_['ndx'] == str(sec.nr) and s_['type'] != 'SECTION':
                entries.add(s_['val'])

        for nr, other in obj.sections.items():
            if 'A' not in other.flags or other.name == '.eh_frame':
                continue
            for r in obj.relocs.get(nr, []):
                if nr == sec.nr and (r['offset'] & ~3) in branch_rel:
                    continue        # an edge or a call target above
                sym = obj.symbols.get(r['info'] >> 32)
                if sym and sym['ndx'] == str(sec.nr):
                    entries.add(sym['val'] + r['addend'])
        # Switch proofs, data words and entry seeding settle together.  An
        # entry that is no function entry starts with nothing known, r2
        # included, so a dispatch such an entry can reach has no bounded
        # table address: its proof goes (head included).  Only the table
        # words of proofs that stand are data: no edge leads into or out of
        # them, so as roots they reach nothing.
        # Every instruction with no code predecessor, and every one no root
        # reaches over code, is such an entry.  Repeat until nothing changes.
        base = set(entries)
        fn_roots = {0} | calls | {s_['val'] for s_ in obj.symbols.values()
                                  if s_['ndx'] == str(sec.nr) and s_['type'] == 'FUNC'}
        proofs, banned, seeded = {}, set(), set()

        def with_edges(wild):
            out = {k: list(v) for k, v in preds.items()}
            for j in indirect:
                if j in proofs:
                    targets = proofs[j][1]
                elif wild:
                    targets = range(0, size, 4)
                else:
                    continue
                for d in targets:
                    out.setdefault(d, []).append(j)
            return out

        def code_succ(data):
            succ_of = {}
            for d, ps in with_edges(False).items():
                for p_ in ps:
                    if p_ not in data and d not in data:
                        succ_of.setdefault(p_, []).append(d)
            return succ_of

        def reach(starts, succ_of):
            seen, todo = set(), [c for c in starts if 0 <= c < size]
            while todo:
                c = todo.pop()
                if c in seen:
                    continue
                seen.add(c)
                todo.extend(succ_of.get(c, []))
            return seen

        settled = False
        for _ in range(2 * (size // 4) + 4):
            entries = base | seeded
            proofs.clear()
            for j in indirect:
                if j not in banned:
                    proof = self.switch_targets(obj, sec, src, size, rels, preds, entries, j)
                    if proof:
                        proofs[j] = proof
            for _i in range(4 * len(indirect) + 1):
                full = with_edges(True)
                # A proof holds only while nothing else enters its sequences.
                broken = [j for j, (expect, _, _) in proofs.items()
                          if any(c in entries or sorted(full.get(c, [])) != ps for c, ps in expect.items())]
                if not broken:
                    break
                # Prove a broken jump again over the edges proven so far: a case
                # that falls into another dispatch is one more path into it.
                known = with_edges(False)
                for j in broken:
                    proof = self.switch_targets(obj, sec, src, size, rels, known, entries, j)
                    if proof and proof != proofs[j]:
                        proofs[j] = proof
                    else:
                        del proofs[j]
            else:
                proofs.clear()
            data = {t + 4 * i for j in proofs for t, n in proofs[j][2] for i in range(n)}
            succ_of = code_succ(data)
            reached_unknown = reach(entries - fn_roots, succ_of)
            drop = {j for j in proofs if j in reached_unknown}
            if drop:
                banned |= drop
                continue
            has_pred = {d for ds in succ_of.values() for d in ds}
            no_pred = {off for off in range(0, size, 4) if off not in has_pred and off not in data}
            seen = reach(entries | no_pred, succ_of)
            new = (no_pred | {off for off in range(0, size, 4) if off not in seen}) - data
            if new <= seeded:
                settled = True
                break
            seeded |= new
        if not settled:
            proofs.clear()
            seeded = set(range(0, size, 4))
        entries = base | seeded
        full = with_edges(True)
        unknown = unknown or next((f"computed jump through CTR at +0x{j:x} (no proven switch)"
                                   for j in indirect if j not in proofs), None)
        if not unknown and returns:
            data = {t + 4 * i for j in proofs for t, n in proofs[j][2] for i in range(n)}
            roots = {0} | calls | {s_['val'] for s_ in obj.symbols.values()
                                   if s_['ndx'] == str(sec.nr) and s_['type'] == 'FUNC'}
            # Every other entry (named, landing pad, or no modelled predecessor)
            # starts unknown; only proven switch-table words are not code.
            unknown = self.prove_returns(src, size, with_edges(False), roots, entries - roots - data,
                                         data, returns)
        fl = {'preds': full, 'entries': entries, 'bad': bad, 'unknown': unknown}
        self.flow_cache[key] = fl
        return fl

    def prove_returns(self, src, size, preds, roots, other_entries, data, returns):
        """
        None when every blr in returns provably returns to the caller, or
        the reason one may not (then it is a computed jump).  A blr returns
        when LR holds the entry LR there: on paths with no LR write (a leaf),
        or after the ABI epilogue: mflr rX; rX stored to the LR save slot
        (entry SP + LR_SLOT_OFF, LR_SLOT_WIDTH bytes) before any call; rY
        reloaded from that slot at a known SP; mtlr rY with rY unchanged;
        blr with no LR write between.  The stack pointer is followed only
        through stdu/stwu r1,-N(r1) and addi r1,r1,N.  No other store
        through r1 may overlap the slot, and a store through r1 at an unknown
        offset counts as overlapping.  Facts that differ where paths merge,
        and every entry that is not a function entry, are unknown.
        Assumed: the program keeps the ABI's LR-save-slot contract (nothing
        writes the slot through a base other than r1).  Every entry that is
        no function entry (a named address, a landing pad, any instruction
        no modelled edge reaches) starts with everything unknown and merges
        like any other path, so a blr it reaches is no proven return.
        """
        UNK = ('unk', 'unk', frozenset(), frozenset(), 'unk')
        ROOT = (0, 'entry', frozenset(), frozenset(), 'none')    # sp, lr, mflr copies, slot reloads, slot
        slot = (LR_SLOT_OFF, LR_SLOT_OFF + LR_SLOT_WIDTH)

        def merge(a, b):
            if a is None:
                return b
            return tuple(x if x == y else u for x, y, u in zip(a, b, UNK))

        def wrote(sp, copies, reloads, regs):
            """The one write effect: the written registers hold neither an
            mflr copy nor a slot reload, and a write of r1 makes SP unknown
            (callers that follow r1 by a constant pass it applied already)."""
            regs = set(regs)
            return ('unk' if 1 in regs else sp), copies - regs, reloads - regs

        def step(off, st):
            sp, lr, copies, reloads, sl = st
            w = be(src, off, 4)
            op, rt, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            xo = (w >> 1) & 0x3ff
            spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
            if op == 31 and xo == 339 and spr == 8:                  # mflr rt
                sp, copies, reloads = wrote(sp, copies, reloads, {rt})
                if lr == 'entry' and sl == 'none' and rt != 1:
                    copies = copies | {rt}
                return sp, lr, copies, reloads, sl
            if op == 31 and xo == 467 and spr == 8:                  # mtlr rt (no GPR write)
                return sp, ('entry' if rt in reloads else 'unk'), copies, reloads, sl
            if op in (16, 18) or (op == 19 and xo in (16, 528)):
                if w & 1:                                            # a call: LR, volatile registers
                    sp, copies, reloads = wrote(sp, copies, reloads, VOLATILE_GPRS)
                    return sp, 'unk', copies, reloads, 'late' if sl == 'none' else sl
                return st
            written = gpr_writes(w)
            if written is None:
                return UNK
            sw = stack_write(w)
            if sw is not None:
                tracked = False
                if sw[0] == 'unknown' or sp == 'unk':
                    sl = 'unk' if sl != 'none' else 'late'
                else:
                    lo = sp + sw[1]
                    hi = lo + sw[2]
                    if (lo, hi) == slot and op == 62 and rt in copies and sl == 'none':
                        sl = 'saved'                                 # the original save
                    if sw[0] == 'update':
                        sp = sp + sw[1]                              # stdu/stwu ...,d(r1) moves r1
                        tracked = True
                sp, copies, reloads = wrote(sp, copies, reloads, set(written) - ({1} if tracked else set()))
                return sp, lr, copies, reloads, sl
            if op == 14 and rt == 1 and ra == 1:                     # addi r1,r1,N
                return (sp + sext16(w & 0xffff) if sp != 'unk' else 'unk'), lr, copies, reloads, sl
            if op == 58 and w & 3 == 0 and ra == 1 and rt != 1:      # ld rt,d(r1)
                d = sext16(w & 0xfffc)
                ok = sp != 'unk' and sl == 'saved' and (sp + d, sp + d + 8) == slot
                sp, copies, reloads = wrote(sp, copies, reloads, {rt})
                return sp, lr, copies, (reloads | {rt}) if ok else reloads, sl
            sp, copies, reloads = wrote(sp, copies, reloads, written)
            return sp, lr, copies, reloads, sl

        state = {}
        work = sorted(roots | other_entries)
        for r_ in roots:
            state[r_] = ROOT
        for e in other_entries:
            state[e] = merge(state.get(e), UNK)
        succ = {}
        for d, ps in preds.items():
            for p_ in ps:
                succ.setdefault(p_, []).append(d)
        rounds = 0
        while work:
            rounds += 1
            if rounds > 64 * (size // 4 + 1):
                return "the return proof did not settle"
            n = work.pop()
            if n in data or n not in state:
                continue
            out = step(n, state[n])
            for d in succ.get(n, []):
                if d in data:
                    continue
                new = merge(state.get(d), out)
                if d in roots:
                    new = merge(new, ROOT)
                if state.get(d) != new:
                    state[d] = new
                    work.append(d)
        # No store through r1 anywhere in the function may reach the slot,
        # except the proven save.
        for off in range(0, size, 4):
            if off in data or off not in state:
                continue
            w = be(src, off, 4)
            sw = stack_write(w)
            if sw is None:
                continue
            sp = state[off][0]
            if sw[0] == 'unknown' or sp == 'unk':
                return f"a store through r1 at +0x{off:x} may write the LR save slot, so a blr is a computed jump that"
            lo = sp + sw[1]
            if lo < slot[1] and slot[0] < lo + sw[2]:
                # Only the store the step above took as the original save.
                if not (state[off][4] == 'none' and step(off, state[off])[4] == 'saved'):
                    return f"a store at +0x{off:x} overlaps the LR save slot, so a blr is a computed jump that"
        for r_ in returns:
            st = state.get(r_)
            if st is None or st[1] != 'entry':
                return f"blr at +0x{r_:x}: LR is not provably the entry LR there, a computed jump that"
        return None

    def toc_table(self, obj, sec, toc_sym, addend, width):
        """
        The in-section address a .toc entry holds, when the entry of this
        width is one relocation naming a local symbol of sec; else None.
        """
        sym = obj.symbols.get(toc_sym)
        nrs = obj.by_name.get('.toc', [])
        if sym is None or len(nrs) != 1 or sym['ndx'] != str(nrs[0]):
            return None
        at = sym['val'] + addend
        want = {4: 'R_PPC64_ADDR32', 8: 'R_PPC64_ADDR64'}[width]
        fw = {'R_PPC64_ADDR32': 4, 'R_PPC64_UADDR32': 4, 'R_PPC64_ADDR16': 2}
        hit = [r for r in obj.relocs.get(nrs[0], [])
               if r['offset'] < at + width and at < r['offset'] + fw.get(r['rtype'], 8)]
        if len(hit) != 1 or hit[0]['offset'] != at or hit[0]['rtype'] != want:
            return None
        tsym = obj.symbols.get(hit[0]['info'] >> 32)
        if tsym is None or tsym['ndx'] != str(sec.nr) or tsym['bind'] != 'LOCAL':
            return None
        return tsym['val'] + hit[0]['addend']

    def switch_targets(self, obj, sec, src, size, rels, preds, entries, j):
        """
        Prove the targets of the computed jump at +j as a switch.  The
        straight-line sequence ending at it (each instruction entered only
        from the one before) and, when its first instruction has several
        predecessors, each predecessor's own straight-line sequence, are
        run symbolically path by path; every path must reach the jump with
        CTR = T + table[i] for a bounded i.  A register holds the low n
        bits of a value (zero-extended); a range check bounds a value on the
        edge where it holds:
            cmplwi/cmpldi crF,rX,K, then bgt crF not taken (or ble taken)
            twi/tdi lgt rX,K (falling through the trap)
            clrlwi / clrldi        narrow a value to its low bits
            sldi, rldic rA,rS,2,MB, slwi
                                   4 * (the low bits of a value) kept
            the table address T from a .toc entry naming a local label of
            this section (addis rH,r2,HA ; lwz/ld rB,LO(rH), or one TOC16
            load from r2)
            lwax rE,rB,rS ; add rT,rE,rB ; mtctr rT ; bctr
        The scaled index is bounded only by a check of at least the bits it
        keeps: a 32-bit check bounds rldic rA,rS,2,30 (32 bits kept) but not
        sldi of the whole register, whose upper half nothing here proves.
        Two loads of one address are two values.  The K+1 table words must lie in the
        section, unrelocated, each an offset from T to an instruction of the
        section.  Returns ({offset: exact predecessors the proof relies on},
        targets) or None.
        """
        def straight(end):
            seq = [end]
            cur = end
            while cur not in entries and preds.get(cur) == [cur - 4] and len(seq) < 64:
                cur -= 4
                seq.insert(0, cur)
            return seq

        chain = straight(j)
        head = chain[0]
        expect = {c: [c - 4] for c in chain[1:]}
        if head in entries or not preds.get(head):
            paths = [chain]
        else:
            hp = sorted(preds[head])
            if len(hp) > 8 or any(p in chain for p in hp):
                return None
            expect[head] = hp
            paths = []
            for p in hp:
                seg = straight(p)
                expect.update({c: [c - 4] for c in seg[1:]})
                paths.append(seg + chain)
        at = {}
        for r in rels:
            at.setdefault(r['offset'] & ~3, []).append(r)
        table = None
        top = -1
        for path in paths:
            got = self.run_switch_path(obj, sec, src, path, at)
            if got is None or (table is not None and got[0] != table):
                return None
            table, top = got[0], max(top, got[1])
        t, k = table, top
        end = t + 4 * (k + 1)
        if t % 4 or t < 0 or end > size or any(t <= r['offset'] < end for r in rels):
            return None
        targets = set()
        for n in range(k + 1):
            d = t + ((be(src, t + 4 * n, 4) ^ 0x80000000) - 0x80000000)
            if not (0 <= d < size and d % 4 == 0):
                return None
            targets.add(d)
        return expect, sorted(targets), [(t, k + 1)]

    def run_switch_path(self, obj, sec, src, path, at):
        """Symbolic run of one path ending at a bcctr: (T, K) when CTR = T + table[i <= K], else None."""
        regs, crs, bounds = {}, {}, {}
        ctr = None
        ids = [0]

        def get(g):
            if g not in regs:
                ids[0] += 1
                regs[g] = ('v', ids[0], 64)
            return regs[g]

        def kill(written):
            for g in written:
                regs.pop(g, None)

        def bound(v, wide, k):
            """v's value is <= k in its low `wide` bits."""
            _, vid, n = v
            m = min(n, wide)
            bounds[(vid, m)] = min(k, bounds.get((vid, m), k))

        def bound_of(vid, n):
            best = None
            for (v, m), k in bounds.items():
                if v == vid and m >= n:
                    k = min(k, (1 << n) - 1)
                    best = k if best is None else min(best, k)
            return best

        for i, off in enumerate(path[:-1]):
            nxt = path[i + 1]
            w = be(src, off, 4)
            op, rt, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            xo = (w >> 1) & 0x3ff
            here = at.get(off, [])
            if op == 10 and not (w >> 22) & 1 and not here:            # cmplwi, cmpldi
                v = get(ra)
                crs[(w >> 23) & 7] = (v, 64 if (w >> 21) & 1 else 32, w & 0xffff) if v[0] == 'v' else None
                continue
            if op in (2, 3) and rt == 1 and not here:                    # tdi/twi lgt: falling through bounds rA
                v = get(ra)
                k = sext16(w & 0xffff)
                if v[0] == 'v' and k >= 0:
                    bound(v, 64 if op == 2 else 32, k)
                continue
            if op == 16 and not w & 1:                                   # bc, not a call
                bo, bi = (w >> 21) & 31, (w >> 16) & 31
                if not bo & 4:
                    return None                                          # decrements CTR
                tgt = off + sext16(w & 0xfffc) if not here else None
                fall = nxt == off + 4 and tgt != off + 4
                taken = tgt is not None and nxt == tgt and tgt != off + 4
                c = crs.get(bi // 4)
                # GT clear on this edge: bgt not taken, or ble taken.
                if c and bi % 4 == 1 and ((bo == 12 and fall) or (bo == 4 and taken)):
                    bound(*c)
                continue
            if op == 15 and ra == 2 and len(here) == 1 and here[0]['rtype'] == 'R_PPC64_TOC16_HA' \
                    and here[0]['offset'] == off + 2:
                kill({rt})
                regs[rt] = ('ha', here[0]['info'] >> 32, here[0]['addend'])
                continue
            load = {32: 4}.get(op) if op != 58 else (8 if w & 3 == 0 else None)
            if load and len(here) == 1 and here[0]['offset'] == off + 2 and w & 0xffff == 0:
                r = here[0]
                src_ok = ((r['rtype'] in ('R_PPC64_TOC16_LO', 'R_PPC64_TOC16_LO_DS') and ra != 0
                           and regs.get(ra) == ('ha', r['info'] >> 32, r['addend']))
                          or (r['rtype'] in ('R_PPC64_TOC16', 'R_PPC64_TOC16_DS') and ra == 2))
                t = self.toc_table(obj, sec, r['info'] >> 32, r['addend'], load) if src_ok else None
                kill({rt})
                if t is not None:
                    regs[rt] = ('table', t)
                continue
            if here:
                kill(set(range(32)))
                crs.clear()
                ctr = None
                continue
            if op == 21 and not w & 1:                                    # rlwinm: clrlwi, slwi
                v = get(rt)
                sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
                kill({ra})
                if v[0] != 'v' or mb > me:
                    continue                                              # a wrapping mask: width unknown
                if sh == 0 and me == 31:
                    regs[ra] = ('v', v[1], min(v[2], 32 - mb))
                elif sh == 2 and me == 29:
                    regs[ra] = ('scaled', v[1], min(v[2], 30 - mb))
                continue
            if op == 30 and not w & 1 and (w >> 2) & 7 in (0, 1, 2):      # rldicl, rldicr, rldic
                v = get(rt)
                sh = rb | (((w >> 1) & 1) << 5)
                f = (w >> 5) & 63
                mbe = ((f >> 1) & 31) | ((f & 1) << 5)
                form = (w >> 2) & 7
                kill({ra})
                if v[0] != 'v':
                    continue
                if form == 0 and sh == 0:                                  # clrldi
                    regs[ra] = ('v', v[1], min(v[2], 64 - mbe))
                elif form == 1 and sh == 2 and mbe == 61:                  # sldi 2
                    regs[ra] = ('scaled', v[1], min(v[2], 62))
                elif form == 2 and sh == 2 and mbe <= 61:                  # rldic 2,MB; MB > 61 wraps
                    regs[ra] = ('scaled', v[1], min(v[2], 62 - mbe))
                continue
            if op == 31 and not w & 1 and xo in (341, 266, 444, 467):
                if xo == 467:                                             # mtspr
                    spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                    if spr == 9:
                        ctr = regs.get(rt)
                    continue
                if xo == 444:                                             # or (mr when rS == rB)
                    v = get(rt) if rt == rb else None
                    kill({ra})
                    if v:
                        regs[ra] = v
                    continue
                va, vb = regs.get(ra), regs.get(rb)
                kill({rt})
                pair = {x[0]: x for x in (va, vb) if x} if ra else {}
                if xo == 341 and 'table' in pair and 'scaled' in pair and len(pair) == 2:       # lwax
                    k = bound_of(pair['scaled'][1], pair['scaled'][2])
                    if k is not None:
                        regs[rt] = ('entry', pair['table'][1], k)
                elif xo == 266 and 'table' in pair and 'entry' in pair \
                        and pair['table'][1] == pair['entry'][1]:                             # add
                    regs[rt] = ('target', pair['entry'][1], pair['entry'][2])
                continue
            written = gpr_writes(w)
            if written is None or (op in (16, 18, 19) and w & 1):
                kill(set(range(32)))
                crs.clear()
                ctr = None
                continue
            kill(written)
            if may_write_cr(w):
                crs.clear()
        if not ctr or ctr[0] != 'target':
            return None
        return ctr[1], ctr[2]

    def reaching_ha(self, obj, sec, src, use_off, reg, key, ha_at):
        """
        The TOC16_HA sites whose addis may supply reg at use_off: every path
        back from the use must end at an addis reg,r2 of the same target
        before any other write of reg and before any entry point.
        ([relocation index], None) or (None, reason).
        """
        fl = self.flow(obj, sec, src)
        if fl['bad']:
            return None, fl['bad']
        if fl.get('unknown'):
            return None, f"{fl['unknown']} may enter any instruction of the section with r{reg} unknown"
        if use_off in fl['entries']:
            return None, f"the use can be entered with r{reg} unknown (+0x{use_off:x} is an entry point)"
        stack = list(fl['preds'][use_off])
        found, seen = set(), set()
        while stack:
            p = stack.pop()
            if p in seen:
                continue
            seen.add(p)
            h = ha_at.get(p)
            if h is not None and h[1]['reg'] == reg:
                hr = h[1]['reloc']
                if (hr['info'] >> 32, hr['addend']) != key:
                    return None, f"r{reg} may come from the TOC16_HA at +0x{p:x} of another target"
                found.add(h[0])
                continue
            writes = gpr_writes(be(src, p, 4))
            if writes is None or reg in writes:
                return None, f"r{reg} may change at +0x{p:x} on a path to this use"
            if p in fl['entries']:
                return None, f"a path from the entry point +0x{p:x} reaches this use without setting r{reg}"
            stack.extend(fl['preds'][p])
        return sorted(found), None

    def check_access(self, obj, sec, src, base, i, r, role, ha_info, ha_at):
        self.counts['accesses'] += 1
        ds = r['rtype'] in DS_RELOCS
        insn_off = r['offset'] - 2
        w = be(src, insn_off, 4)
        form = classify_insn(w, ds)
        where = f"{obj.ident} {sec.name}+0x{insn_off:x}"

        def done(state, msg, pairs=()):
            self.dispose(obj, sec, i, state, msg, r)
            for pair in pairs:
                pair['used'].append((state, msg))

        if form is None:
            return done('refused', f"{where}: {r['rtype']} on an instruction form v1 does not model (0x{w:08x})")
        kind, width = form
        ra = (w >> 16) & 31
        rt = (w >> 21) & 31
        pairs = [None]
        if role == 'lo':
            if ra == 0:
                return done('refused', f"{where}: {r['rtype']} on RA=0, which is literal zero: no TOC16_HA pairs with it")
            if ra == 2:
                return done('refused', f"{where}: {r['rtype']} based on r2 without its TOC16_HA")
            found, why = self.reaching_ha(obj, sec, src, insn_off, ra, (r['info'] >> 32, r['addend']), ha_at)
            if why:
                return done('refused', f"{where}: {why}")
            pairs = [ha_info[j] for j in found]
        elif ra != 2:
            return done('refused', f"{where}: {r['rtype']} not based on r2")
        used = [p for p in pairs if p is not None]
        target = self.resolve(obj, r['info'] >> 32, r['addend'])
        if target[0] == 'refuse':
            return done('refused', f"{where}: {target[1]}", used)
        out_w = self.elf.read(base + insn_off, 4)
        if out_w is None:
            return done('failed', f"{where}: 0x{base + insn_off:x} is not in a file-backed section", used)
        ow = be(out_w, 0, 4)
        # With several HA sites reaching the use, each must give the right access.
        results = [self.evaluate(obj, base, w, ow, ds, kind, width, ra, rt, target, pair) for pair in pairs]
        for state in ('failed', 'refused', 'verified'):
            hit = [m for st, m in results if st == state]
            if hit:
                return done(state, f"{where}: {hit[0]}" if hit[0] else None, used)

    def evaluate(self, obj, base, w, ow, ds, kind, width, ra, rt, target, pair):
        """Decode one access in the ELF, with one of its HA sites: (state, msg)."""
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
                return 'refused', f"its addis became 0x{(hw or 0):08x}, a rewrite v1 does not model"
        o_op, o_rt, o_ra = ow >> 26, (ow >> 21) & 31, (ow >> 16) & 31
        rewritten_addi = False
        if o_op == (w >> 26) and o_rt == rt and (not ds or (ow & 3) == (w & 3)):
            pass
        elif ds and kind == 'load' and width == 8 and o_op == 14 and o_rt == rt:
            rewritten_addi = True
        else:
            return 'refused', f"the ELF has 0x{ow:08x} for 0x{w:08x}, a rewrite v1 does not model"
        if o_ra != expect_ra:
            if pair is not None and expect_ra == 2 and o_ra == ra:
                return 'failed', f"its addis is gone but the access still uses r{ra}"
            return 'refused', f"the ELF bases the access on r{o_ra}, expected r{expect_ra}"
        disp = sext16(ow) if rewritten_addi else disp_of(ow, ds)
        ea = (self.elf.toc + (hi << 16) + disp) & 0xffffffffffffffff

        if target[0] == 'toc':
            if rewritten_addi:
                return self.check_toc_addi(obj, target[1], ea)
            if kind == 'load':
                return self.check_toc_read(obj, target[1], width, ea)
            return 'refused', f"{kind} on a .toc entry"
        if rewritten_addi:
            return 'refused', "addi rewrite of a direct (non-.toc) load"
        if target[0] == 'content' and kind == 'load':
            _, tobj, tsec, mbase, delta = target
            start, data = self.merged_entry(tobj, tsec, mbase)
            if start is None:
                return 'refused', data
            lo = mbase + delta - start
            if lo < 0 or lo + width > len(data):
                return 'refused', f"{width}-byte load at {tsec.name}+0x{mbase:x}{delta:+d} leaves its merged entry"
            ok, msg = self.content_matches(target, ea)
            return ('verified', None) if ok else ('failed', f"load at 0x{ea:x}: {msg}")
        ok, m = self.value_ok(target, ea, 8)
        return ('verified', None) if ok else ('failed', f"address 0x{ea:x}: {m}")

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


COVERAGE_LIMITS = """coverage limits (refused, by design):
  a dispatch that range-checks one load and indexes with a second load of
  the same address (two loads are never assumed equal); switch (int)
  lowered as cmplwi + sldi (no ABI-extension rule: a 32-bit check does not
  bound a shift of the whole register); and every section with a transfer
  through LR or CTR that is not a proven switch or a provable return
  (back-chain restores ld/lwz r1,0(r1) among them).
assumed: the program keeps the ABI LR-save-slot contract (nothing writes
  the slot through a base other than r1).  Every instruction no modelled
  edge reaches (landing pads, orphan code) is a possible entry with
  nothing known."""


def main():
    p = argparse.ArgumentParser(description="Check a linked PPU ELF's TOC accesses against its input objects.",
                                epilog=COVERAGE_LIMITS, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture', help="run a link and pin its map, trace and inputs")
    c.add_argument('--out', required=True, help="fresh directory for the manifest, map and trace")
    c.add_argument('command', nargs=argparse.REMAINDER, help="-- followed by the link command")
    k = sub.add_parser('check', help="check a captured link", epilog=COVERAGE_LIMITS,
                       formatter_class=argparse.RawDescriptionHelpFormatter)
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
