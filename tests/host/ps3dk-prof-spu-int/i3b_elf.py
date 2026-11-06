#!/usr/bin/env python3
"""I3b ELF tooling (design P3.2 I3b r2 + review 09:49): a synthetic SPU ELF writer, the INDEPENDENT acceptance checker
(written from the design rules, not from psp_elf.c), the named single-rule corpus and the seeded mutations.

The checker's verdict line format is the one the C harness prints:
  <name> EINVAL
  <name> OK bid=<hex> n=<n>
  <name> R <i> <ls_addr> <line> <name hex> <file hex>      (one line per record, after OK)

usage: i3b_elf.py corpus OUTDIR [--fixtures ELF...] [--mutations N --seed S]
       i3b_elf.py oracle FILE... | oracle-list LISTFILE (prints verdicts)"""
import os
import random
import re
import struct
import sys

SIZE_MAX = 16 << 20
PHNUM_MAX, SHNUM_MAX, NOTE_MAX, LS_END = 64, 4096, 4096, 0x40000
DESC, NOTE = b'ps3dkprof_spudesc', b'.note.gnu.build-id'


# ---------------------------------------------------------------- the checker
def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def text(b, off, end, lo, hi):
    """printable ASCII 0x20..0x7E, NUL before end, length lo..hi; None if not"""
    n = 0
    while True:
        if off + n >= end or n > hi:
            return None
        c = b[off + n]
        if c == 0:
            break
        if c < 0x20 or c > 0x7E:
            return None
        n += 1
    return b[off:off + n] if n >= lo else None


def oracle(b):
    """None = EINVAL, else (build_id bytes, [(ls_addr, line, name, file)])"""
    size = len(b)
    if size < 52 or size > SIZE_MAX:
        return None
    if b[:4] != b'\x7fELF' or b[4] != 1 or b[5] != 2 or b[6] != 1:
        return None
    if u16(b, 16) != 2 or u16(b, 18) != 23 or u32(b, 20) != 1 or u16(b, 40) != 52:
        return None
    phoff, shoff = u32(b, 28), u32(b, 32)
    phentsize, phnum, shentsize, shnum, shstrndx = (u16(b, o) for o in (42, 44, 46, 48, 50))
    if phnum == 0xFFFF or not 1 <= phnum <= PHNUM_MAX or phentsize != 32 or phoff + 32 * phnum > size:
        return None
    if shnum == 0 or shnum > SHNUM_MAX or shentsize != 40 or shoff + 40 * shnum > size:
        return None
    if shstrndx >= 0xFF00 or shstrndx >= shnum:
        return None
    loads = []
    for i in range(phnum):
        o = phoff + 32 * i
        p_type, p_off, p_va, _, p_fsz, p_msz, _, p_al = struct.unpack_from('>8I', b, o)
        if p_type != 1:
            continue
        if p_fsz > p_msz or p_off + p_fsz > size or p_va + p_msz > LS_END:
            return None
        if p_al > 1 and ((p_al & (p_al - 1)) or p_va % p_al != p_off % p_al):
            return None
        for (q_off, q_va, q_fsz, q_msz) in loads:
            if p_va < q_va + q_msz and q_va < p_va + p_msz:
                return None
        loads.append((p_off, p_va, p_fsz, p_msz))
    sh = [struct.unpack_from('>10I', b, shoff + 40 * i) for i in range(shnum)]
    st = sh[shstrndx]
    if st[1] != 3:
        return None
    str_off, str_sz = st[4], st[5]
    if str_sz < 1 or str_off + str_sz > size:
        return None
    strtab = b[str_off:str_off + str_sz]
    last_nul = strtab.rfind(b'\0')
    if last_nul < 0:
        return None
    desc_i = note_i = None
    for i, s in enumerate(sh):
        if s[0] > last_nul:
            return None
        nm = strtab[s[0]:strtab.index(b'\0', s[0])]
        if nm == DESC:
            if desc_i is not None:
                return None
            desc_i = i
        elif nm == NOTE:
            if note_i is not None:
                return None
            note_i = i
    if desc_i is None:
        return None

    def mapped(va, ln):
        for (p_off, p_va, p_fsz, _) in loads:
            if p_va <= va and va - p_va <= p_fsz and ln <= p_fsz - (va - p_va):
                return p_off + (va - p_va), p_off + p_fsz
        return None

    d = sh[desc_i]
    addr, off, dsz = d[3], d[4], d[5]
    if d[1] != 1 or not (d[2] & 2) or dsz % 16 or not 1 <= dsz // 16 <= 4096 or addr % 16:
        return None
    if addr + dsz > LS_END or off + dsz > size:
        return None
    m = mapped(addr, dsz)
    if m is None or m[0] != off:
        return None
    for s in sh:
        if s[1] in (4, 9) and s[7] == desc_i:
            return None
    bid = None
    if note_i is not None:
        s = sh[note_i]
        n_off, n_sz = s[4], s[5]
        if s[1] != 7 or n_sz < 12 or n_sz > NOTE_MAX or n_off + n_sz > size:
            return None
        namesz, descsz, ntype = struct.unpack_from('>3I', b, n_off)
        pad = lambda x: (x + 3) & ~3
        if namesz != 4 or descsz > NOTE_MAX or n_sz != 12 + pad(namesz) + pad(descsz):
            return None
        if ntype != 3 or b[n_off + 12:n_off + 16] != b'GNU\0':
            return None
        if any(b[n_off + 16 + descsz:n_off + 16 + pad(descsz)]):
            return None
        if 1 <= descsz <= 63:
            bid = b'\x01' + b[n_off + 16:n_off + 16 + descsz]
    if bid is None:
        h = 0xcbf29ce484222325
        for c in b:
            h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        bid = b'\x02' + h.to_bytes(8, 'big')
    recs = []
    for i in range(dsz // 16):
        ls, line, pn, pf = struct.unpack_from('>4I', b, off + 16 * i)
        if ls != addr + 16 * i:
            return None
        mn, mf = mapped(pn, 1), mapped(pf, 1)
        if mn is None or mf is None:
            return None
        nm, fl = text(b, mn[0], mn[1], 1, 512), text(b, mf[0], mf[1], 0, 512)
        if nm is None or fl is None:
            return None
        recs.append((ls, line, nm, fl))
    return bid, recs


def verdict(name, b):
    r = oracle(b)
    if r is None:
        return [f'{name} EINVAL']
    bid, recs = r
    out = [f'{name} OK bid={bid.hex()} n={len(recs)}']
    for i, (ls, line, nm, fl) in enumerate(recs):
        out.append(f'{name} R {i} {ls} {line} {nm.hex()} {fl.hex()}')
    return out


# ---------------------------------------------------------------- the run judge (shared by the driver)
LINE = re.compile(r'^(\S+) (EINVAL|OK bid=[0-9a-f]+ n=(\d+)|R (\d+) \d+ \d+ [0-9a-f]+ [0-9a-f]*)$')


def judge_run(rc, stdout, stderr, names):
    """the per-file verdicts of a C harness run, or (None, reason): fail closed on anything but a complete, clean run
    whose every file is EXACTLY one EINVAL alone, or one OK header with n=K followed by K records indexed 0..K-1"""
    if rc != 0:
        return None, f'rc {rc}'
    if 'I3B PASS' not in stdout:
        return None, 'no PASS marker'
    if 'runtime error' in stderr or 'AddressSanitizer' in stderr:
        return None, 'a sanitizer report'
    by = {}
    for l in stdout.splitlines():
        if not l or l.startswith('I3B'):
            continue
        m = LINE.match(l)
        if not m:
            return None, f'malformed line {l[:80]!r}'
        by.setdefault(m.group(1), []).append(m)
    if set(by) != set(names):
        return None, f'inventory: {len(set(names) - set(by))} files missing, {len(set(by) - set(names))} unexpected'
    for nm, ms in by.items():
        head = ms[0].group(2)
        if head == 'EINVAL':
            if len(ms) != 1:
                return None, f'{nm}: EINVAL with {len(ms) - 1} further lines'
            continue
        if ms[0].group(3) is None:
            return None, f'{nm}: no OK header first'
        k = int(ms[0].group(3))
        idx = [int(m.group(4)) if m.group(4) is not None else -1 for m in ms[1:]]
        if idx != list(range(k)):
            return None, f'{nm}: n={k} but records {idx[:6]}{"..." if len(idx) > 6 else ""} (missing, duplicate or extra)'
    return {nm: [m.group(0) for m in ms] for nm, ms in by.items()}, ''


def judge_controls():
    """three runs the judge must reject: a failed empty run, rc-0 output truncated by whole files, and rc-0 output
    with every file present but one record missing inside a file"""
    good = ['a OK bid=02aa n=2', 'a R 0 1 2 61 62', 'a R 1 3 4 63 64', 'b EINVAL', 'I3B PASS']
    names = {'a', 'b'}
    ok, _ = judge_run(0, '\n'.join(good), '', names)
    res = [('the well-formed reference run is accepted', ok is not None)]
    for label, rc, out in (('failed process, empty output', 1, ''),
                           ('rc 0, whole files truncated', 0, '\n'.join(good[:3] + good[4:])),
                           ('rc 0, all names present, one record missing', 0, '\n'.join(good[:2] + good[3:])),
                           ('rc 0, a duplicate record', 0, '\n'.join(good[:3] + [good[2]] + good[3:])),
                           ('rc 0, EINVAL followed by a record', 0, '\n'.join(good + ['b R 0 1 2 61 62']))):
        r, why = judge_run(rc, out, '', names)
        res.append((f'{label} is rejected ({why})', r is None))
    return res


# ---------------------------------------------------------------- the synthetic writer
class Spec:
    """a minimal valid SPU EXEC: one PT_LOAD at vaddr == file offset (congruent), strings + records in it"""

    def __init__(self, names=(b'zone_a', b'zone_b'), files=(b'a.c', b'b.c')):
        self.names, self.files = list(names), list(files)
        self.base = 0x200          # vaddr == file offset of the segment
        self.extra_loads = []      # (off, va, fsz, msz, align)
        self.note = None           # bytes of the note section, or None
        self.notes = 1             # how many build-id sections
        self.descs = 1             # how many descriptor sections
        self.rel_on_desc = None    # 4 (RELA) or 9 (REL)
        self.patch = []            # (offset, bytes) applied last
        self.desc_type, self.desc_flags = 1, 2
        self.strtab_type = 3
        self.self_off = 0
        self.records = None        # override record tuples
        self.extra_sh = 0          # extra SHT_NULL section headers (the shnum limit cases)
        self.desc_copy = False     # sh_offset points at an identical copy of the records outside the segment


def build(sp):
    nph = 1 + len(sp.extra_loads)
    sp.base = max(sp.base, (52 + 32 * nph + 15) & ~15)  # the segment starts after the program-header table
    seg = bytearray()
    ptrs = []
    for nm, fl in zip(sp.names, sp.files):
        pn = sp.base + len(seg)
        seg += nm + b'\0'
        pf = sp.base + len(seg)
        seg += fl + b'\0'
        ptrs.append((pn, pf))
    while len(seg) % 16:
        seg += b'\0'
    desc_addr = sp.base + len(seg)
    recs = sp.records if sp.records is not None else [
        (desc_addr + 16 * i + sp.self_off, 10 + i, pn, pf) for i, (pn, pf) in enumerate(ptrs)]
    for r in recs:
        seg += struct.pack('>4I', *r)
    desc_size = 16 * len(recs)
    # file layout: ehdr, phdrs, padding to base, the segment, note, shstrtab, rel, section headers
    nph = 1 + len(sp.extra_loads)
    out = bytearray(sp.base)
    out[52:52 + 32 * nph] = b'\0' * (32 * nph)
    out += seg
    seg_end = len(out)
    note_off = len(out)
    if sp.note is not None:
        out += sp.note
    names = [b'', b'.shstrtab'] + [DESC] * sp.descs + ([NOTE] * sp.notes if sp.note is not None else []) + \
        ([b'.rela.desc'] if sp.rel_on_desc else [])
    strtab = bytearray(b'')
    offs = []
    for nm in names:
        offs.append(len(strtab))
        strtab += nm + b'\0'
    str_off = len(out)
    out += strtab
    copy_off = len(out)
    if sp.desc_copy:  # the same record bytes again, outside the PT_LOAD: the header and the segment disagree
        out += seg[desc_addr - sp.base:]
    rel_off = len(out)
    if sp.rel_on_desc:
        out += b'\0' * 12
    while len(out) % 4:
        out += b'\0'
    shoff = len(out)
    shdrs = [(0,) * 10, (offs[1], sp.strtab_type, 0, 0, str_off, len(strtab), 0, 0, 1, 0)]
    k = 2
    desc_index = 2
    for _ in range(sp.descs):
        shdrs.append((offs[k], sp.desc_type, sp.desc_flags, desc_addr,
                      copy_off if sp.desc_copy else sp.base + (desc_addr - sp.base), desc_size, 0, 0, 16, 0))
        k += 1
    if sp.note is not None:
        for _ in range(sp.notes):
            shdrs.append((offs[k], 7, 2, 0, note_off, len(sp.note), 0, 0, 4, 0))
            k += 1
    if sp.rel_on_desc:
        shdrs.append((offs[k], sp.rel_on_desc, 0, 0, rel_off, 12, 0, desc_index, 4, 12))
    shdrs += [(0,) * 10] * sp.extra_sh
    for s in shdrs:
        out += struct.pack('>10I', *s)
    hdr = b'\x7fELF' + bytes([1, 2, 1, 0]) + b'\0' * 8
    hdr += struct.pack('>HHIIIIIHHHHHH', 2, 23, 1, 0, 52, shoff, 0, 52, 32, nph, 40, len(shdrs), 1)
    out[0:52] = hdr
    loads = [(sp.base, sp.base, len(seg), len(seg), 16)] + sp.extra_loads
    for i, (o, va, fsz, msz, al) in enumerate(loads):
        out[52 + 32 * i:52 + 32 * i + 32] = struct.pack('>8I', 1, o, va, va, fsz, msz, 7, al)
    for o, bts in sp.patch:
        out[o:o + len(bts)] = bts
    return bytes(out)


def note(descsz, desc=None, namesz=4, name=b'GNU\0', ntype=3, padbyte=0, extra=b''):
    desc = desc if desc is not None else bytes((i * 7 + 1) & 0xFF for i in range(descsz))
    pad = lambda x: (x + 3) & ~3
    n = struct.pack('>3I', namesz, descsz, ntype) + name.ljust(pad(len(name)), b'\0')
    n += desc + bytes([padbyte]) * (pad(descsz) - descsz)
    return n + extra


def corpus():
    """(name, bytes, expected 'OK'|'EINVAL'): every rule singly, exact/one-over limits, the controls"""
    cs = []

    def add(name, sp, exp, post=None):
        b = build(sp)
        if post:
            b = post(bytearray(b))
        cs.append((name, bytes(b), exp))

    add('valid-minimal', Spec(), 'OK')
    # the api harness's image: 4 records (distinct first/middle/last copy-pass failures: gets 5, 6, 8), 48 string
    # bytes, so its arena need (4 descriptors + 48) keeps an exact fit in an 8-aligned arena
    add('api-exact', Spec(names=[b'zone_aa', b'zone_bb', b'zone_cc', b'zone_dd'], files=[b'a.c', b'b.c', b'c.c', b'd.c']),
        'OK')
    add('valid-buildid-20', Spec(), 'OK', None)
    cs[-1] = ('valid-buildid-20', build(sp_note(20)), 'OK')
    cs.append(('valid-buildid-63', build(sp_note(63)), 'OK'))
    cs.append(('buildid-64-fallback', build(sp_note(64)), 'OK'))
    cs.append(('buildid-0-fallback', build(sp_note(0)), 'OK'))
    cs.append(('buildid-bad-name', build(sp_note(20, name=b'GNX\0')), 'EINVAL'))
    cs.append(('buildid-bad-type', build(sp_note(20, ntype=4)), 'EINVAL'))
    cs.append(('buildid-nonzero-pad', build(sp_note(21, padbyte=1)), 'EINVAL'))
    cs.append(('buildid-two-notes', build(sp_note(20, extra=note(4))), 'EINVAL'))
    cs.append(('buildid-trailing-bytes', build(sp_note(20, extra=b'\0\0\0\0')), 'EINVAL'))
    s = sp_note(20)
    s.notes = 2
    cs.append(('buildid-two-sections', build(s), 'EINVAL'))
    add('name-printable-max-512', Spec(names=[b'n' * 512], files=[b'f.c']), 'OK')
    add('name-513', Spec(names=[b'n' * 513], files=[b'f.c']), 'EINVAL')
    add('file-512', Spec(names=[b'z'], files=[b'f' * 512]), 'OK')
    add('file-513', Spec(names=[b'z'], files=[b'f' * 513]), 'EINVAL')
    add('file-empty', Spec(names=[b'z'], files=[b'']), 'OK')
    add('name-empty', Spec(names=[b''], files=[b'f.c']), 'EINVAL')
    add('name-control-char', Spec(names=[b'ta\tb'], files=[b'f.c']), 'EINVAL')
    add('name-high-byte', Spec(names=[b'z\x80'], files=[b'f.c']), 'EINVAL')
    add('records-4096', Spec(names=[b'z'] * 4096, files=[b''] * 4096), 'OK')
    add('records-4097', Spec(names=[b'z'] * 4097, files=[b''] * 4097), 'EINVAL')
    sp = Spec()
    sp.self_off = 16
    add('self-address-off-by-16', sp, 'EINVAL')
    add('descs-two-sections', _set(Spec(), descs=2), 'EINVAL')
    add('desc-not-progbits', _set(Spec(), desc_type=8), 'EINVAL')
    add('desc-not-alloc', _set(Spec(), desc_flags=0), 'EINVAL')
    add('desc-write-alloc-ok', _set(Spec(), desc_flags=3), 'OK')
    add('desc-shoffset-disagrees', _set(Spec(), desc_copy=True), 'EINVAL')
    add('rela-on-desc', _set(Spec(), rel_on_desc=4), 'EINVAL')
    add('rel-on-desc', _set(Spec(), rel_on_desc=9), 'EINVAL')
    add('shstrtab-not-strtab', _set(Spec(), strtab_type=1), 'EINVAL')
    # header fields
    for nm, off, val in (('bad-magic', 0, b'\x7fELG'), ('class64', 4, b'\x02'), ('lsb', 5, b'\x01'),
                         ('ei-version-2', 6, b'\x02'), ('type-rel', 16, b'\x00\x01'), ('machine-ppc', 18, b'\x00\x15'),
                         ('e-version-0', 20, b'\0\0\0\0'), ('ehsize-64', 40, b'\x00\x40'),
                         ('phentsize-56', 42, b'\x00\x38'), ('shentsize-64', 46, b'\x00\x40'),
                         ('phnum-xnum', 44, b'\xff\xff'), ('phnum-0', 44, b'\x00\x00'), ('shnum-0', 48, b'\x00\x00'),
                         ('shstrndx-xindex', 50, b'\xff\xff'), ('shstrndx-loreserve', 50, b'\xff\x00'),
                         ('shstrndx-out-of-range', 50, b'\x00\x40'), ('phoff-overflow', 28, b'\xff\xff\xff\xf0'),
                         ('shoff-overflow', 32, b'\xff\xff\xff\xf0')):
        sp = Spec()
        sp.patch = [(off, val)]
        add(nm, sp, 'EINVAL')
    # segments
    add('load-overlap', _set(Spec(), extra_loads=[(0x200, 0x210, 0x10, 0x10, 16)]), 'EINVAL')
    add('load-bad-align', _set(Spec(), extra_loads=[(0x10, 0x30000, 0x10, 0x10, 24)]), 'EINVAL')
    add('load-incongruent', _set(Spec(), extra_loads=[(0x10, 0x30004, 0x10, 0x10, 16)]), 'EINVAL')
    add('load-ls-overflow', _set(Spec(), extra_loads=[(0x10, 0x3fff0, 0x10, 0x20, 16)]), 'EINVAL')
    add('load-ls-exact-end', _set(Spec(), extra_loads=[(0x10, 0x3fff0, 0x10, 0x10, 16)]), 'OK')
    add('load-fsz-gt-msz', _set(Spec(), extra_loads=[(0x10, 0x30000, 0x20, 0x10, 16)]), 'EINVAL')
    add('load-va-wrap', _set(Spec(), extra_loads=[(0x10, 0xFFFFFFF0, 0x10, 0x20, 16)]), 'EINVAL')
    add('shnum-4096', _set(Spec(), extra_sh=4096 - 3), 'OK')
    add('shnum-4097', _set(Spec(), extra_sh=4097 - 3), 'EINVAL')
    cs.append(('note-section-4096', build(sp_note(4080)), 'OK'))       # 12 + 4 + 4080: the cap, fallback id
    cs.append(('note-section-4100-over-cap', build(sp_note(4084)), 'EINVAL'))
    add('phnum-64', _set(Spec(), extra_loads=[(0, 0, 0, 0, 0)] * 63), 'OK', None)
    add('phnum-65', _set(Spec(), extra_loads=[(0, 0, 0, 0, 0)] * 64), 'EINVAL', None)
    # truncation at every header/table boundary of a valid image
    base = build(Spec())
    shoff = u32(base, 32)
    for cut in (0, 4, 51, 52, 52 + 31, 0x200, shoff, shoff + 40, len(base) - 1):
        cs.append((f'truncated-{cut}', base[:cut], 'EINVAL'))
    # an unrelated malformed section name (offset past the last NUL)
    b2 = bytearray(base)
    struct.pack_into('>I', b2, shoff, 0xFFFF)
    cs.append(('unrelated-name-offset-bad', bytes(b2), 'EINVAL'))
    b3 = bytearray(base)
    st_off = u32(base, shoff + 40 + 16)
    st_sz = u32(base, shoff + 40 + 20)
    b3[st_off + st_sz - 1] = ord('x')  # the last name loses its NUL
    cs.append(('shstrtab-last-nul-gone', bytes(b3), 'EINVAL'))
    # size cap
    big = bytearray(build(Spec()))
    cs.append(('size-cap-exact', bytes(big + b'\0' * (SIZE_MAX - len(big))), 'OK'))
    cs.append(('size-cap-one-over', bytes(big + b'\0' * (SIZE_MAX + 1 - len(big))), 'EINVAL'))
    cs.append(('size-under-header-51', base[:51], 'EINVAL'))
    return cs


def sp_note(descsz, **kw):
    s = Spec()
    s.note = note(descsz, **kw)
    return s


def _set(sp, **kw):
    for k, v in kw.items():
        setattr(sp, k, v)
    return sp


def mutations(seeds, n, seed):
    rnd = random.Random(seed)
    out = []
    for k in range(n):
        name, b = seeds[k % len(seeds)]
        b = bytearray(b)
        for _ in range(rnd.choice((1, 1, 2, 3))):
            mode = rnd.random()
            if mode < 0.5:  # a header/table byte
                o = rnd.randrange(0, min(len(b), 0x300))
            elif mode < 0.8:  # anywhere
                o = rnd.randrange(0, len(b))
            else:  # the section-header table
                sh = u32(b, 32) if len(b) >= 36 else 0
                o = min(len(b) - 1, sh + rnd.randrange(0, 40 * 8))
            b[o] = rnd.randrange(256)
        out.append((f'mut-{k:05d}-{name}', bytes(b)))
    return out


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    if a[0] == 'judge-controls':
        res = judge_controls()
        for label, ok in res:
            print(f'judge control: {label}: {"PASS" if ok else "FAIL"}')
        return 0 if all(ok for _, ok in res) else 1
    if a[0] in ('oracle', 'oracle-list'):
        paths = a[1:] if a[0] == 'oracle' else [l.strip() for l in open(a[1]) if l.strip()]
        for f in paths:
            nm = os.path.basename(f)
            nm = nm[:-4] if nm.endswith('.elf') else nm  # the C harness's naming
            print('\n'.join(verdict(nm, open(f, 'rb').read())))
        return 0
    if a[0] == 'corpus':
        out = a[1]
        os.makedirs(out, exist_ok=False)
        fixtures = a[a.index('--fixtures') + 1:] if '--fixtures' in a else []
        fixtures = [f for f in fixtures if not f.startswith('--')]
        nmut = int(a[a.index('--mutations') + 1]) if '--mutations' in a else 0
        seed = int(a[a.index('--seed') + 1]) if '--seed' in a else 1
        bad = []
        listing = []
        for name, b, exp in corpus():
            got = 'EINVAL' if oracle(b) is None else 'OK'
            if got != exp:
                bad.append(f'{name}: oracle {got}, expected {exp}')
            open(os.path.join(out, name + '.elf'), 'wb').write(b)
            listing.append(f'{name} {exp}')
        seeds = [('minimal', build(Spec())), ('buildid', build(sp_note(20)))]
        for f in fixtures:
            b = open(f, 'rb').read()
            nm = 'fx-' + os.path.basename(os.path.dirname(f)) + '-' + os.path.basename(f).replace('.elf', '')
            open(os.path.join(out, nm + '.elf'), 'wb').write(b)
            listing.append(f'{nm} FIXTURE')
            seeds.append((nm, b))
        for nm, b in mutations(seeds, nmut, seed):
            open(os.path.join(out, nm + '.elf'), 'wb').write(b)
            listing.append(f'{nm} MUTATION')
        open(os.path.join(out, 'LISTING.txt'), 'w').write('\n'.join(listing) + '\n')
        for x in bad:
            print(f'ORACLE SELF-CHECK FAIL {x}')
        print(f'corpus: {len(listing)} files ({len(corpus())} named, {len(fixtures)} fixtures, {nmut} mutations); '
              f'oracle self-check {"FAIL" if bad else "PASS"}')
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
