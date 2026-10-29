#!/usr/bin/env python3
"""
Host test for tools/toc-linked-check.py on synthetic links.

The model linker in toc_linked_fixture.py writes objects, archives, an
executable, a map, a trace and a manifest; each case checks the exit
status and the reported counts or reason.  Doctored cases edit the
executable, map or trace after a correct link, so a check that ignores
the doctored property passes them and the case fails.

Needs a readelf that reads big-endian ELF64 PowerPC (GNU readelf does):
PPU_READELF, then powerpc64-ps3-elf-readelf, then readelf on PATH.
"""

import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

NL = chr(10)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# TOC_LINKED_TOOL runs the cases against another copy (to show a case red on an older tool).
TOOL = os.environ.get('TOC_LINKED_TOOL') or os.path.join(REPO, 'tools', 'toc-linked-check.py')

_spec = importlib.util.spec_from_file_location('toc_linked_fixture', os.path.join(HERE, 'toc_linked_fixture.py'))
F = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(F)
from_obj = F.Obj


def find_readelf():
    for cand in (os.environ.get('PPU_READELF'), shutil.which('powerpc64-ps3-elf-readelf'),
                 shutil.which('readelf')):
        if cand and os.path.isfile(cand):
            return cand
    for base in (os.environ.get('PS3DK'), os.environ.get('PS3DEV')):
        if base:
            for name in ('powerpc64-ps3-elf-readelf.exe', 'powerpc64-ps3-elf-readelf'):
                p = os.path.join(base, 'ppu', 'bin', name)
                if os.path.isfile(p):
                    return p
    return None


READELF = None
WORK = None
_counter = [0]


def fresh(name):
    _counter[0] += 1
    d = os.path.join(WORK, f"{_counter[0]:02d}-{name}")
    os.makedirs(d)
    return d


def save(obj, d, fname=None):
    p = os.path.join(d, fname or obj.name)
    with open(p, 'wb') as f:
        f.write(obj.to_bytes())
    return p


def check(manifest):
    res = subprocess.run([sys.executable, '-I', '-B', TOOL, 'check', manifest, '--readelf', READELF, '-v'],
                         capture_output=True, text=True)
    counts = {}
    for line in res.stdout.splitlines():
        m = re.match(r'^  (\w+)\s+(\d+)$', line)
        if m:
            counts[m.group(1)] = int(m.group(2))
    status = None
    m = re.search(r'^toc-linked-check: (PASS|FAIL|INCOMPLETE)$', res.stdout, re.M)
    if m:
        status = m.group(1)
    return res.returncode, status, counts, res.stdout + res.stderr


# ---------------------------------------------------------------------------
# Fixture builders

def data_obj(name='data.o', globals_=(('g_a', 0x11111111), ('g_b', 0x22222222))):
    """A data object defining 8-byte globals with distinct contents."""
    o = from_obj(name)
    blob = b''.join(struct.pack('>Q', v) for _, v in globals_)
    o.section('.data', blob, flags='WA', align=8)
    for k, (n, _) in enumerate(globals_):
        o.sym(n, '.data', 8 * k, bind='GLOBAL', stype='OBJECT', size=8)
    return o


def toc_user(name='a.o', medium=True):
    """
    f: addis r9,r2,HA(.toc+0); ld r9,LO(.toc+0)(r9)   -> &g_a  (medium model)
       ld r10,.toc+8(r2)                               -> &g_b  (small model)
       blr
    .toc: [0] ADDR64 g_a, [8] ADDR64 g_b
    """
    o = from_obj(name)
    words = []
    if medium:
        words += [F.addis(9, 2), F.ld(9, 9)]
    words += [F.ld(10, 2), F.BLR]
    o.section('.text.f', F.code(*words))
    o.sym('f', '.text.f', 0, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(16), flags='WA', align=8)
    o.sym('.LC0', '.toc', 0)
    o.sym('.LC1', '.toc', 8)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.sym('g_b', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.toc', 8, 'R_PPC64_ADDR64', 'g_b')
    if medium:
        o.rel('.text.f', 2, 'R_PPC64_TOC16_HA', '.LC0')
        o.rel('.text.f', 6, 'R_PPC64_TOC16_LO_DS', '.LC0')
        o.rel('.text.f', 10, 'R_PPC64_TOC16_DS', '.LC1')
    else:
        o.rel('.text.f', 2, 'R_PPC64_TOC16_DS', '.LC1')
    return o


def link(d, objs, **kw):
    inputs = [(o, save(o, d), None) for o in objs]
    return F.Link(inputs, **kw).run(d)


def text_addr(lk, oname, sec):
    return lk.place[(oname, sec)]


# ---------------------------------------------------------------------------
# Cases.  Each returns (ok, note).

def want(res, rc, status, **counts):
    """(ok, note) for a check result against an exit status, status and counts."""
    got_rc, got_status, c, out = res
    if (got_rc, got_status) != (rc, status):
        return False, f"rc={got_rc} {got_status}, expected rc={rc} {status}\n{out}"
    for k, v in counts.items():
        if c.get(k) != v:
            return False, f"{k}={c.get(k)}, expected {v}\n{out}"
    return True, ', '.join(f"{k} {c[k]}" for k in ('sites_verified', 'sites_failed', 'sites_refused') if k in c)


def want_detail(res, rc, status, pattern, **counts):
    ok, note = want(res, rc, status, **counts)
    if ok and not re.search(pattern, res[3]):
        return False, f"no detail matching {pattern!r}\n{res[3]}"
    return ok, note


def case_green():
    d = fresh('green')
    lk = link(d, [toc_user(), data_obj()])
    rc, st, c, out = check(lk.manifest_path)
    if (rc, st) != (0, 'PASS') or c.get('sites_verified') != 3 or c.get('sites_refused') != 0:
        return False, out
    return True, f"verified {c['sites_verified']}"


# -- defect regressions ----------------------------------------------------

def merged_string_user(addend=-1):
    """
    Address formation of .LC0 - 1 in a merged string section, where .LC0 is
    the section's first string (compilers emit this for pre-increment string
    walks).  ld resolves a non-section symbol in a merged section to the
    symbol's own output address plus the addend.
    """
    o = from_obj('s.o')
    o.section('.text.s', F.code(F.addis(9, 2), F.addi(9, 9), F.BLR))
    o.sym('s', '.text.s', 0, bind='GLOBAL', stype='FUNC')
    o.section('.rodata.s.str1.8', b'hello\0\0\0world\0\0\0', flags='AMS', align=8, entsize=1)
    o.sym('.LC0', '.rodata.s.str1.8', 0)
    o.sym('.LC1', '.rodata.s.str1.8', 8)
    o.rel('.text.s', 2, 'R_PPC64_TOC16_HA', '.LC0', addend)
    o.rel('.text.s', 6, 'R_PPC64_TOC16_LO', '.LC0', addend)
    return o


def padded_merged_link(d):
    pad = from_obj('pad.o')
    pad.section('.rodata.pad', b'xyzxyzxy', flags='A', align=8)   # non-NUL bytes right in front of the string
    lk = link(d, [pad, merged_string_user()])
    if lk.place[('s.o', '.rodata.s.str1.8')] != lk.place[('pad.o', '.rodata.pad')] + 8:
        raise RuntimeError("fixture: the string does not follow the pad")
    return lk


def case_merged_string_negative_addend():
    d = fresh('merged-neg')
    lk = padded_merged_link(d)

    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=2, sites_failed=0, sites_refused=0)


def case_merged_string_negative_addend_wrong():
    d = fresh('merged-neg-wrong')
    lk = padded_merged_link(d)
    a = lk.place[('s.o', '.text.s')] + 4
    w = lk.word(a)
    lk.poke_word(a, (w & ~0xffff) | ((w - 1) & 0xffff))   # .LC0 - 2
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'does not hold the datum', sites_failed=2)


LONG = '.text._Z' + 'N3foo3barIJ' * 40 + 'E1fEv'


def case_long_section_name():
    d = fresh('long-name')
    o = toc_user(medium=False)
    o.sections[0].name = LONG
    o.relocs[LONG] = o.relocs.pop('.text.f')
    lk = link(d, [o, data_obj()])
    if not any(l.strip() == LONG for l in lk.map_lines):
        return False, "fixture: the map does not wrap the long name onto its own line"
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=1, sites_refused=0)


def branch_user(variant):
    """
    The addis and its use are joined only through a branch; the fall-through
    path between them writes the register (as in newlib's _malloc_r).
      0 addis r4,r2,HA(.LC0)
      4 beq  +0xc            -> 0x10
      8 li   r4,9            (not on the path to the use)
      c blr
     10 ld   r9,LO(.LC0)(r4)
     14 blr
    variants: 'ok'; 'bypass' (0xc becomes b +4: the clobber reaches the use);
    'entry' (a symbol at 0x10: the use can be entered with r4 unknown).
    """
    o = from_obj('m.o')
    w = [F.addis(4, 2), F.beq(0xc), F.li(4, 9), F.BLR, F.ld(9, 4), F.BLR]
    if variant == 'bypass':
        w[3] = F.b(4)
    o.section('.text.m', F.code(*w))
    o.sym('m', '.text.m', 0, bind='GLOBAL', stype='FUNC')
    if variant == 'entry':
        o.sym('m_alt', '.text.m', 0x10, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(8), flags='WA', align=8)
    o.sym('.LC0', '.toc', 0)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.text.m', 2, 'R_PPC64_TOC16_HA', '.LC0')
    o.rel('.text.m', 0x12, 'R_PPC64_TOC16_LO_DS', '.LC0')
    return o


def case_branch_around_clobber():
    d = fresh('branch-ok')
    lk = link(d, [branch_user('ok'), data_obj()])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=2, sites_refused=0)


def case_branch_bypass_refused():
    d = fresh('branch-bypass')
    lk = link(d, [branch_user('bypass'), data_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'r4 may change at \+0x8', sites_refused=2)


def case_branch_entry_refused():
    d = fresh('branch-entry')
    lk = link(d, [branch_user('entry'), data_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'entered', sites_refused=2)


# -- the design review's negative list ----------------------------------------

def green_link(name, **kw):
    d = fresh(name)
    return link(d, [toc_user(), data_obj()], **kw)


def f_addr(lk):
    return lk.place[('a.o', '.text.f')]


def case_wrong_displacement_relocs_agree():
    # Move the small-model ld onto the other entry and emit an output
    # relocation that agrees with the moved displacement: the checker must
    # judge the instruction against the input objects, not the output's own
    # relocations.
    lk = green_link('wrong-disp')
    a = f_addr(lk) + 8
    w = lk.word(a)
    lk.poke_word(a, (w & ~0xfffc) | ((w - 8) & 0xfffc))
    lk.out_relas = {lk.out_index('.text'): [(a + 2, F.R['R_PPC64_TOC16_DS'], 0, lk.out['.toc'][0] - lk.toc)]}
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'holds the wrong value', sites_failed=1, sites_verified=2)


def case_stale_map_placement():
    d = fresh('stale-map')
    z = from_obj('z.o')                      # code after .text.f, so the shift stays inside .text
    z.section('.text.z', F.code(F.li(3, 1), F.li(4, 2), F.li(5, 3), F.BLR))
    lk = link(d, [toc_user(), z, data_obj()])
    old = '0x%016x' % f_addr(lk)
    new = '0x%016x' % (f_addr(lk) + 4)

    def shift(lines):
        return [l.replace(old, new) if '.text.f' in l else l for l in lines]
    lk.edit_map(shift)
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'stale or shifted placement', sites_refused=3)


def case_missing_map_placement():
    lk = green_link('missing-map')
    lk.edit_map(lambda lines: [l for l in lines if not l.startswith(' .text.f ')])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'map neither places nor discards [.]text[.]f', sites_refused=3)


def case_discarded_section():
    d = fresh('discarded')
    a = toc_user()
    a.section('.text.dead', F.code(F.ld(11, 2), F.BLR))
    a.rel('.text.dead', 2, 'R_PPC64_TOC16_DS', '.LC0')
    lk = link(d, [a, data_obj()], discard=[('a.o', '.text.dead')])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=3, sites_discarded=1, sites_refused=0)


def two_users():
    """Two objects with the same local .toc labels naming different globals."""
    a = toc_user('a.o', medium=False)
    b = from_obj('b.o')
    b.section('.text.f2', F.code(F.ld(10, 2), F.BLR))
    b.sym('f2', '.text.f2', 0, bind='GLOBAL', stype='FUNC')
    b.section('.toc', bytes(16), flags='WA', align=8)
    b.sym('.LC0', '.toc', 0)
    b.sym('.LC1', '.toc', 8)
    b.sym('g_a', 'UND', bind='GLOBAL')
    b.sym('g_b', 'UND', bind='GLOBAL')
    b.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_b')
    b.rel('.toc', 8, 'R_PPC64_ADDR64', 'g_a')
    b.rel('.text.f2', 2, 'R_PPC64_TOC16_DS', '.LC1')
    return a, b


def case_duplicate_local_names():
    d = fresh('dup-locals')
    a, b = two_users()
    lk = link(d, [a, b, data_obj()])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=2, sites_refused=0)


def case_duplicate_local_names_swapped():
    # b.o's .LC1 (g_a) read through a.o's .LC1 (g_b): same label, wrong object.
    d = fresh('dup-locals-swap')
    a, b = two_users()
    lk = link(d, [a, b, data_obj()])
    at = lk.place[('b.o', '.text.f2')]
    w = lk.word(at)
    lk.poke_word(at, (w & ~0xfffc) | ((lk.place[('a.o', '.toc')] + 8 - lk.toc) & 0xfffc))
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'b[.]o [.]text[.]f2', sites_failed=1, sites_verified=1)


def case_duplicate_section_names():
    d = fresh('dup-sections')
    a = toc_user(medium=False)
    a.section('.text.f', F.code(F.BLR))      # a second .text.f (as with section groups)
    a.relocs = {(1 if k == '.text.f' else k): v for k, v in a.relocs.items()}
    lk = link(d, [a, data_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'has 2 sections named [.]text[.]f', sites_refused=1)


def case_archive_member_ambiguity():
    d = fresh('ar-ambiguous')
    a = toc_user(medium=False)
    dup = from_obj('x.o')
    dup.section('.text.x', F.code(F.BLR))
    lib = os.path.join(d, 'lib.a')
    F.write_archive(lib, [('a.o', a.to_bytes()), ('a.o', dup.to_bytes())])
    data = data_obj()
    lk = F.Link([(a, lib, 'a.o'), (data, save(data, d), None)]).run(d)
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'cannot tell them apart', sites_refused=1)


def case_archive_member_green():
    d = fresh('ar-green')
    a = toc_user()
    lib = os.path.join(d, 'lib.a')
    F.write_archive(lib, [('a.o', a.to_bytes())])
    data = data_obj()
    lk = F.Link([(data, save(data, d), None), (a, lib, 'a.o')]).run(d)
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=3, sites_refused=0)


def low_word_user():
    """lwz r3,.LC0+4(r2): the low word of an 8-byte entry (an ILP32 address read)."""
    o = from_obj('a.o')
    o.section('.text.f', F.code(F.lwz(3, 2), F.BLR))
    o.sym('f', '.text.f', 0, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(16), flags='WA', align=8)
    o.sym('.LC0', '.toc', 8)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.sym('g_b', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_b')
    o.rel('.toc', 8, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.text.f', 2, 'R_PPC64_TOC16', '.LC0', 4)
    return o


def case_low_word_of_entry():
    d = fresh('k4')
    lk = link(d, [low_word_user(), data_obj()])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=1)


def case_low_word_of_entry_wrong():
    d = fresh('k4-wrong')
    lk = link(d, [low_word_user(), data_obj()])
    a = f_addr(lk)
    lk.poke_word(a, lk.word(a) - 4)          # reads the high word instead
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'4-byte read of [.]toc[+]0xc .* holds the wrong value', sites_failed=1)


def carry_user():
    """An entry 0x8000 past .TOC.: HA carries (1) and LO is negative (-0x8000)."""
    o = from_obj('a.o')
    o.section('.text.f', F.code(F.addis(9, 2), F.ld(9, 9), F.BLR))
    o.sym('f', '.text.f', 0, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(0x10008), flags='WA', align=8)
    o.sym('.LCfar', '.toc', 0x10000)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.rel('.toc', 0x10000, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.text.f', 2, 'R_PPC64_TOC16_HA', '.LCfar')
    o.rel('.text.f', 6, 'R_PPC64_TOC16_LO_DS', '.LCfar')
    return o


def case_lo_carry():
    d = fresh('carry')
    lk = link(d, [carry_user(), data_obj()])
    if lk.word(f_addr(lk)) & 0xffff != 1 or lk.word(f_addr(lk) + 4) & 0xffff != 0x8000:
        return False, "fixture: the access does not carry into HA"
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=2)


def case_lo_carry_dropped():
    d = fresh('carry-dropped')
    lk = link(d, [carry_user(), data_obj()])
    lk.poke_word(f_addr(lk), F.addis(9, 2, 0))   # HA without the carry
    lk.rewrite()
    return want(check(lk.manifest_path), 1, 'FAIL', sites_failed=2)


def case_ds_form_changed():
    lk = green_link('ds-bits')
    a = f_addr(lk) + 8
    lk.poke_word(a, lk.word(a) | 1)          # ld -> ldu
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'a rewrite v1 does not model', sites_refused=1)


def case_nop_rewrite():
    # ld's TOC edit: addis r9,r2,0 -> nop, and the paired ld rebased on r2.
    lk = green_link('nop-ok')
    a = f_addr(lk)
    if lk.word(a) & 0xffff:
        return False, "fixture: HA is not zero"
    lk.poke_word(a, F.NOP)
    w = lk.word(a + 4)
    lk.poke_word(a + 4, (w & ~(31 << 16)) | (2 << 16))
    lk.rewrite()
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=3)


def case_nop_rewrite_wrong():
    lk = green_link('nop-wrong')
    a = f_addr(lk)
    lk.poke_word(a, F.NOP)                   # the paired ld still uses r9
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'its addis is gone', sites_failed=2)


def addi_rewrite(lk, delta=0):
    # ld r10,.LC1(r2) -> addi r10,r2,(g_b - .TOC.), as ld does for an entry
    # whose value is near the TOC.
    a = f_addr(lk) + 8
    lk.poke_word(a, F.addi(10, 2, lk.globals['g_b'] - lk.toc + delta))
    lk.rewrite()


def case_addi_rewrite():
    lk = green_link('addi-ok')
    addi_rewrite(lk)
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=3)


def case_addi_rewrite_wrong():
    lk = green_link('addi-wrong')
    addi_rewrite(lk, 8)
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'addi rewrite of [.]toc[+]0x8', sites_failed=1)


def case_read_outside_file():
    # Point the small-model ld into .bss, which has no file contents.
    d = fresh('nobits')
    data = data_obj()
    data.section('.bss', flags='WA', stype='NOBITS', size=0x40, align=8)
    lk = link(d, [toc_user(), data])
    a = f_addr(lk) + 8
    w = lk.word(a)
    lk.poke_word(a, (w & ~0xfffc) | ((lk.out['.bss'][0] - lk.toc) & 0xfffc))
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 1, 'FAIL', r'not inside a file-backed section', sites_failed=1)


def case_truncated_elf():
    lk = green_link('truncated')
    with open(lk.elf_path, 'r+b') as f:
        f.truncate(200)
    lk.repin()
    return want(check(lk.manifest_path), 2, None)


def case_unpaired_ha():
    d = fresh('unpaired-ha')
    o = toc_user(medium=False)
    o.sections[0].data = F.code(F.ld(10, 2), F.addis(9, 2), F.BLR)
    o.sections[0].size = 12
    o.rel('.text.f', 6, 'R_PPC64_TOC16_HA', '.LC0')
    lk = link(d, [o, data_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'a[.]o [.]text[.]f[+]0x4: TOC16_HA has no paired access',
                       relocation_sites=2, sites_verified=1, sites_refused=1, sites_unaccounted=0)


def case_shared_ha():
    # One addis feeding two uses: three sites, each consumed exactly once.
    d = fresh('shared-ha')
    o = toc_user()
    o.sections[0].data = F.code(F.addis(9, 2), F.ld(10, 9), F.ld(11, 9), F.BLR)
    o.sections[0].size = 16
    o.relocs['.text.f'] = [(2, 'R_PPC64_TOC16_HA', '.LC0', 0), (6, 'R_PPC64_TOC16_LO_DS', '.LC0', 0),
                           (10, 'R_PPC64_TOC16_LO_DS', '.LC0', 0)]
    lk = link(d, [o, data_obj()])
    return want(check(lk.manifest_path), 0, 'PASS', relocation_sites=3, sites_verified=3, sites_unaccounted=0)


def case_unsupported_relocation():
    d = fresh('unsupported-reloc')
    o = toc_user(medium=False)
    o.sections[0].data = F.code(F.ld(10, 2), F.addis(9, 2), F.BLR)
    o.sections[0].size = 12
    o.rel('.text.f', 6, 'R_PPC64_TOC16_HI', '.LC0')
    lk = link(d, [o, data_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'R_PPC64_TOC16_HI is outside v1',
                       relocation_sites=2, sites_verified=1, sites_refused=1, sites_unaccounted=0)


def case_unknown_form():
    d = fresh('unknown-form')
    o = toc_user(medium=False)
    o.sections[0].data = F.code((46 << 26) | (28 << 21) | (2 << 16), F.BLR)   # lmw r28,0(r2)
    lk = link(d, [o, data_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'instruction form v1 does not model', sites_refused=1)


def case_multiple_toc():
    lk = green_link('multi-toc', opd=[('a.o', 'f')])
    at = lk.out['.opd'][0] + 4
    lk.poke(at, (lk.toc + 0x10000).to_bytes(4, 'big'))
    lk.rewrite()
    return want_detail(check(lk.manifest_path), 2, None, r'multiple TOCs are outside v1')


def case_single_toc_descriptor():
    lk = green_link('one-toc', opd=[('a.o', 'f')])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=3)


def case_stub_section():
    stub = F.Section('.stub', 'PROGBITS', 'AX', F.code(F.NOP), 4, 4, 0, 0x80000)
    lk = green_link('stub', extra_out=[stub])
    return want_detail(check(lk.manifest_path), 2, None, r'linker stub section [.]stub')


def case_zero_coverage():
    d = fresh('zero')
    o = from_obj('a.o')
    o.section('.text.f', F.code(F.BLR))
    o.sym('f', '.text.f', 0, bind='GLOBAL', stype='FUNC')
    lk = link(d, [o])
    return want(check(lk.manifest_path), 1, 'INCOMPLETE', sites_verified=0, relocation_sites=0)


def case_stripped_elf():
    lk = green_link('stripped')
    lk.patch_shdr('.symtab', 4, 4, 0)        # SHT_SYMTAB -> SHT_NULL
    return want_detail(check(lk.manifest_path), 2, None, r'stripped')


def case_missing_map():
    lk = green_link('no-map')
    os.remove(lk.map_path)
    return want_detail(check(lk.manifest_path), 2, None, r'map link[.]map is missing')


def case_wrong_object_set():
    lk = green_link('wrong-objects')
    with open(os.path.join(lk.outdir, 'a.o'), 'ab') as f:
        f.write(bytes(8))
    return want_detail(check(lk.manifest_path), 2, None, r'does not match its pinned SHA-256')


def case_trace_names_other_file():
    lk = green_link('trace-mismatch')
    with open(lk.trace_path, 'a', newline='') as f:
        f.write(lk.file_key(os.path.join(lk.outdir, 'other.o')) + NL)
    lk.repin()
    return want_detail(check(lk.manifest_path), 2, None, r'name different files')


def asm(items):
    """
    Two-pass assembler for the switch fixtures.  items: label strings and
    (name, fn) pairs, fn(off, L) -> word; L maps labels to offsets.
    Returns (words, L, {name: offset}).
    """
    L, at, off = {}, {}, 0
    for it in items:
        if isinstance(it, str):
            L[it] = off
        else:
            at.setdefault(it[0], off)
            off += 4
    words = [it[1](o, L) for o, it in zip(range(0, off, 4), [i for i in items if not isinstance(i, str)])]
    return words, L, at


def switch_user(check=('cmplwi', 3, 1), bc=(12, 1), scale=('rldic', 30), clobber_index=False,
                table_named=True, case0_at='c0', entry_at_mtctr=False, table_last=False,
                wrong_base=False, reenter=None, reload=False, toc32=False):
    """
    A switch as the compiler emits it: a range check, then a computed jump
    through an in-section table of offsets from the table's own address:
         cmplwi r3,1 ; bgt dflt            (the range check; other forms below)
    head: addis r11,r2,HA(.LC0)            (r11 for case 1, set before the jump)
         addis r9,r2,HA(.LC1)
         rldic r3,r3,2,30                  (4 * the low 32 bits: the bits checked)
         ld    r10,LO(.LC1)(r9)            (the table address, from a .toc entry)
         lwax  r9,r10,r3 ; add r9,r9,r10 ; mtctr r9 ; bctr
    T:   .long c0-T, c1-T
    c0:  addis r12,r2,HA(.LC0) ; ld r10,LO(.LC0)(r12) ; blr
    c1:  ld r10,LO(.LC0)(r11) ; b again    (r11 from before the jump)
    again: blr                             (or a second check that re-enters head)
    dflt: blr
    check: ('cmplwi'|'cmpldi', reg, K), ('clr8', K) = clrlwi r10,r3,24 then
    cmplwi r10,K, ('twi', K) = clrlwi r10,r3,28 then twi lgt r10,K, or None.
    scale: ('rldic', MB) or ('sldi',).  reenter: (bo, bi) of a check at
    `again` (cmplwi r3,1 then bc bo,bi,head).  reload: the index is
    reloaded from memory after a check of a first load.  toc32: 4-byte
    ADDR32 .toc entries read with lwz, as the ILP32 libraries have them.
    """
    ldw = F.lwz if toc32 else F.ld
    items = []
    if reload:
        items += [('ld3', lambda o, L: F.lbz(10, 4)), ('cmp', lambda o, L: F.cmplwi(10, 1)),
                  ('bc', lambda o, L: F.bc(12, 1, L['dflt'] - o))]
    elif check and check[0] in ('cmplwi', 'cmpldi'):
        enc = F.cmplwi if check[0] == 'cmplwi' else F.cmpldi
        items += [('cmp', lambda o, L, c=check: enc(c[1], c[2])),
                  ('bc', lambda o, L: F.bc(bc[0], bc[1], L['dflt'] - o))]
    elif check and check[0] == 'clr8':
        items += [('clr', lambda o, L: F.clrlwi(10, 3, 24)), ('cmp', lambda o, L: F.cmplwi(10, check[1])),
                  ('bc', lambda o, L: F.bc(bc[0], bc[1], L['dflt'] - o))]
    elif check and check[0] == 'twi':
        items += [('clr', lambda o, L: F.clrlwi(10, 3, 28)), ('trap', lambda o, L: F.twi_lgt(10, check[1]))]
    if clobber_index:
        items.append(('clobber', lambda o, L: F.addi(3, 3, 1)))
    items.append('head')
    items += [('ha11', lambda o, L: F.addis(11, 2)), ('ha9', lambda o, L: F.addis(9, 2))]
    if reload:
        items.append(('reload', lambda o, L: F.lbz(3, 4)))
    if scale[0] == 'sldi':
        items.append(('scale', lambda o, L: F.sldi(3, 3, 2)))
    else:
        items.append(('scale', lambda o, L: F.rldic(3, 3, 2, scale[1])))
    items.append(('ld10', lambda o, L: ldw(10, 9)))
    if wrong_base:
        items += [('ha12', lambda o, L: F.addis(12, 2)), ('ld12', lambda o, L: ldw(12, 12))]
    items += [('lwax', lambda o, L: F.lwax(9, 10, 3)),
              ('add', lambda o, L: F.add(9, 9, 12 if wrong_base else 10)),
              'mtctr', ('mtctr', lambda o, L: F.mtctr(9)), ('bctr', lambda o, L: F.bctr())]

    def table():
        first = {'c0': lambda L: L['c0'], 'lo': lambda L: L['c0'] + 4, 'mtctr': lambda L: L['mtctr'],
                 'head': lambda L: L['head']}[case0_at]
        return ['T', ('t0', lambda o, L: first(L) - L['T']), ('t1', lambda o, L: L['c1'] - L['T'])]
    body = ['c0', ('c0ha', lambda o, L: F.addis(12, 2)), ('c0lo', lambda o, L: ldw(10, 12)), ('c0r', lambda o, L: F.BLR),
            'c1', ('c1lo', lambda o, L: ldw(10, 11)), ('c1b', lambda o, L: F.b(L['again'] - o)), 'again']
    if reenter:
        body += [('again_cmp', lambda o, L: F.cmplwi(3, 1)),
                 ('again_bc', lambda o, L: F.bc(reenter[0], reenter[1], L['head'] - o))]
    body += [('again_r', lambda o, L: F.BLR), 'dflt', ('dflt_r', lambda o, L: F.BLR)]
    items += body + table() if table_last else table() + body
    words, L, at = asm(items)
    o = from_obj('j.o')
    o.section('.text.j', F.code(*words))
    o.sym('j', '.text.j', 0, bind='GLOBAL', stype='FUNC')
    if entry_at_mtctr:
        o.sym('j2', '.text.j', at['mtctr'], bind='GLOBAL', stype='FUNC')
    es, ent, lo = (4, 'R_PPC64_ADDR32', 'R_PPC64_TOC16_LO') if toc32 else (8, 'R_PPC64_ADDR64', 'R_PPC64_TOC16_LO_DS')
    o.section('.toc', bytes(3 * es), flags='WA', align=8)
    o.sym('.LC0', '.toc', 0)
    o.sym('.LC1', '.toc', es)
    o.sym('.LC2', '.toc', 2 * es)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, ent, 'g_a')
    if table_named:
        o.rel('.toc', es, ent, '.text.j', L['T'])
    else:
        o.rel('.toc', es, ent, 'g_a')
    o.rel('.toc', 2 * es, ent, '.text.j', L['T'] + 4)
    o.rel('.text.j', at['ha9'] + 2, 'R_PPC64_TOC16_HA', '.LC1')
    o.rel('.text.j', at['ld10'] + 2, lo, '.LC1')
    if wrong_base:
        o.rel('.text.j', at['ha12'] + 2, 'R_PPC64_TOC16_HA', '.LC2')
        o.rel('.text.j', at['ld12'] + 2, lo, '.LC2')
    o.rel('.text.j', at['ha11'] + 2, 'R_PPC64_TOC16_HA', '.LC0')
    o.rel('.text.j', at['c0ha'] + 2, 'R_PPC64_TOC16_HA', '.LC0')
    o.rel('.text.j', at['c0lo'] + 2, lo, '.LC0')
    o.rel('.text.j', at['c1lo'] + 2, lo, '.LC0')
    return o


def switch_case(name, **kw):
    d = fresh(name)
    lk = link(d, [switch_user(**kw), data_obj()])
    return check(lk.manifest_path)


def proven(name, **kw):
    return want(switch_case(name, **kw), 0, 'PASS', sites_verified=6, sites_refused=0)


# Without a proven table the jump may reach any instruction: the table load
# and case 0's use are refused through it, and case 1, which no modelled
# edge reaches, is an entry point with unknown registers.

def unproven(name, refused=6, **kw):
    return want(switch_case(name, **kw), 1, 'INCOMPLETE', sites_verified=0, sites_refused=refused)


def case_jump_table():
    # cmplwi + rldic ...,2,30: the 32 bits checked are the 32 bits scaled.
    return proven('switch')


def case_jump_table_toc32():
    # 4-byte .toc entries read with lwz: the table entry's neighbours are
    # other 4-byte entries, not part of it.
    return proven('switch-toc32', toc32=True)


def case_jump_table_cmpldi_sldi():
    # A 64-bit check bounds sldi of the whole register.
    return proven('switch-cmpldi', check=('cmpldi', 3, 1), scale=('sldi',))


def case_jump_table_low_byte():
    # clrlwi r10,r3,24 ; cmplwi r10,K ; rldic r3,r3,2,54 (libstdc++'s shape).
    return proven('switch-low-byte', check=('clr8', 1), scale=('rldic', 54))


def case_jump_table_trap_bound():
    # clrlwi r10,r3,28 ; twi lgt r10,K ; rldic r3,r3,2,58 (libgcc's shape).
    return proven('switch-trap', check=('twi', 1), scale=('rldic', 58))


def case_jump_table_reentered():
    # A second check (ble taken) re-enters the dispatch: both paths bounded.
    return proven('switch-reenter', reenter=(4, 1))


def case_jump_table_reentered_unbounded():
    # The second path enters when the index is OUT of range (bgt taken).
    return unproven('switch-reenter-bad', reenter=(12, 1))


def case_jump_table_sldi_after_32bit_check():
    # A 32-bit check says nothing of the upper half sldi scales.
    return unproven('switch-sldi-32', scale=('sldi',))


def case_jump_table_byte_check_wider_scale():
    # A check of the low byte does not bound 32 scaled bits.
    return unproven('switch-byte-wide', check=('clr8', 1), scale=('rldic', 30))


def case_jump_table_reload():
    # The index is reloaded from memory after a check of a first load: two values.
    return unproven('switch-reload', reload=True)


def nested_switch_user():
    """
    Two dispatches; a case of the first falls into the second, so the
    second's first instruction is entered both by fall-through and by the
    first table:
         cmplwi r3,1 ; bgt dflt
         addis r9,r2,HA(.LC1) ; rldic r3,r3,2,30 ; ld r10,LO(.LC1)(r9)
         lwax r9,r10,r3 ; add r9,r9,r10 ; mtctr r9 ; bctr
    T1:  .long d2-T1, x-T1
    x:   li r5,0                            (falls into d2)
    d2:  clrlwi r10,r4,24 ; cmplwi r10,1 ; bgt dflt
         addis r9,r2,HA(.LC2) ; rldic r4,r4,2,54 ; ld r10,LO(.LC2)(r9)
         lwax r9,r10,r4 ; add r9,r9,r10 ; mtctr r9 ; bctr
    T2:  .long c-T2, c-T2
    c:   addis r12,r2,HA(.LC0) ; ld r10,LO(.LC0)(r12) ; blr
    dflt: blr
    """
    items = [('cmp1', lambda o, L: F.cmplwi(3, 1)), ('bgt1', lambda o, L: F.bgt(L['dflt'] - o)),
             ('ha1', lambda o, L: F.addis(9, 2)), ('sc1', lambda o, L: F.rldic(3, 3, 2, 30)),
             ('ld1', lambda o, L: F.ld(10, 9)), ('lwax1', lambda o, L: F.lwax(9, 10, 3)),
             ('add1', lambda o, L: F.add(9, 9, 10)), ('mtctr1', lambda o, L: F.mtctr(9)),
             ('bctr1', lambda o, L: F.bctr()),
             'T1', ('t10', lambda o, L: L['d2'] - L['T1']), ('t11', lambda o, L: L['x'] - L['T1']),
             'x', ('li', lambda o, L: F.li(5, 0)),
             'd2', ('clr2', lambda o, L: F.clrlwi(10, 4, 24)), ('cmp2', lambda o, L: F.cmplwi(10, 1)),
             ('bgt2', lambda o, L: F.bgt(L['dflt'] - o)),
             ('ha2', lambda o, L: F.addis(9, 2)), ('sc2', lambda o, L: F.rldic(4, 4, 2, 54)),
             ('ld2', lambda o, L: F.ld(10, 9)), ('lwax2', lambda o, L: F.lwax(9, 10, 4)),
             ('add2', lambda o, L: F.add(9, 9, 10)), ('mtctr2', lambda o, L: F.mtctr(9)),
             ('bctr2', lambda o, L: F.bctr()),
             'T2', ('t20', lambda o, L: L['c'] - L['T2']), ('t21', lambda o, L: L['c'] - L['T2']),
             'c', ('cha', lambda o, L: F.addis(12, 2)), ('clo', lambda o, L: F.ld(10, 12)),
             ('cr', lambda o, L: F.BLR), 'dflt', ('dr', lambda o, L: F.BLR)]
    words, L, at = asm(items)
    o = from_obj('n.o')
    o.section('.text.n', F.code(*words))
    o.sym('n', '.text.n', 0, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(24), flags='WA', align=8)
    o.sym('.LC0', '.toc', 0)
    o.sym('.LC1', '.toc', 8)
    o.sym('.LC2', '.toc', 16)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.toc', 8, 'R_PPC64_ADDR64', '.text.n', L['T1'])
    o.rel('.toc', 16, 'R_PPC64_ADDR64', '.text.n', L['T2'])
    for ha, lo, lc in (('ha1', 'ld1', '.LC1'), ('ha2', 'ld2', '.LC2'), ('cha', 'clo', '.LC0')):
        o.rel('.text.n', at[ha] + 2, 'R_PPC64_TOC16_HA', lc)
        o.rel('.text.n', at[lo] + 2, 'R_PPC64_TOC16_LO_DS', lc)
    return o


def case_jump_table_nested():
    # The second dispatch is proven over both paths into it, the first's
    # table edge among them, and neither proof breaks the other.
    d = fresh('switch-nested')
    lk = link(d, [nested_switch_user(), data_obj()])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=6, sites_refused=0)


def review3_switch(scale, ctr_decrement=False, zero_base=False, table1=None, decrement_to_nop=False,
                   bdnz=False, final_bo=20):
    """
    The third review's switch: cmpldi r3,1 ; bgt +0x50 ; addis rB,r2,HA(.LC1)
    ; <scale> ; ld r10,LO(.LC1)(rB) ; lwax r9,r10,r3 ; add r9,r9,r10 ; mtctr r9
    [; bdzlr] ; bctr, a table at +0x60 and three addis/ld pairs at +0x38,
    +0x40, +0x48 (a ld at +0x4c, past its addis, is table[1]'s target
    unless table1 says otherwise).  rB is r9, or r0 with zero_base.
    """
    words = [F.BLR] * 26
    seq = [F.cmpldi(3, 1), F.bgt(80 - 4), F.addis(0 if zero_base else 9, 2), scale,
           F.ld(10, 0 if zero_base else 9), F.lwax(9, 10, 3), F.add(9, 9, 10), F.mtctr(9)]
    if ctr_decrement:
        seq.append(F.NOP if decrement_to_nop else F.bc(16, 0, 4) if bdnz else F.bclr(18, 0))
    seq.append(F.bcctr(final_bo, 0))
    words[:len(seq)] = seq
    for off in (56, 64, 72):
        words[off // 4] = F.addis(12, 2)
        words[off // 4 + 1] = F.ld(11, 12)
    t1 = table1 if table1 is not None else (64 if ctr_decrement else 76)
    words[24:26] = [(64 - 96) & 0xffffffff, (t1 - 96) & 0xffffffff]
    o = from_obj('j.o')
    o.section('.text.j', F.code(*words))
    o.sym('j', '.text.j', 0, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(16), flags='WA', align=8)
    o.sym('.LC0', '.toc', 0)
    o.sym('.LC1', '.toc', 8)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.toc', 8, 'R_PPC64_ADDR64', '.text.j', 96)
    o.rel('.text.j', 10, 'R_PPC64_TOC16_HA', '.LC1')
    o.rel('.text.j', 18, 'R_PPC64_TOC16_LO_DS', '.LC1')
    for off in (56, 64, 72):
        o.rel('.text.j', off + 2, 'R_PPC64_TOC16_HA', '.LC0')
        o.rel('.text.j', off + 6, 'R_PPC64_TOC16_LO_DS', '.LC0')
    return o


def review3(name, obj):
    d = fresh(name)
    lk = link(d, [obj, data_obj()])
    return check(lk.manifest_path)


def case_r3_rldic_wrap():
    # rldic r3,r3,2,62: the mask wraps; its width is unknown, never zero.
    return want(review3('r3-rldic-wrap62', review3_switch(F.rldic(3, 3, 2, 62))), 1, 'INCOMPLETE',
                sites_verified=0, sites_failed=0, sites_refused=8)


def case_r3_rlwinm_wrap():
    # rlwinm r3,r3,2,30,29: MB > ME wraps.
    return want(review3('r3-rlwinm-wrap30', review3_switch(F.rlwinm(3, 3, 2, 30, 29))), 1, 'INCOMPLETE',
                sites_verified=0, sites_failed=0, sites_refused=8)


def case_r3_rldic_normal():
    # Control: rldic r3,r3,2,30 proves both entries; table[1] lands past an addis.
    return want(review3('r3-rldic-normal30', review3_switch(F.rldic(3, 3, 2, 30))), 1, 'INCOMPLETE',
                sites_verified=6, sites_failed=0, sites_refused=2)


def case_r3_rlwinm_normal():
    # Control: rlwinm r3,r3,2,0,29 (slwi 2) does not wrap.
    return want(review3('r3-rlwinm-normal0', review3_switch(F.rlwinm(3, 3, 2, 0, 29))), 1, 'INCOMPLETE',
                sites_verified=6, sites_failed=0, sites_refused=2)


def case_r3_bdzlr():
    # bdzlr decrements CTR even when it falls through: the dispatch is unknown.
    return want(review3('r3-bdzlr', review3_switch(F.rldic(3, 3, 2, 30), ctr_decrement=True)), 1, 'INCOMPLETE',
                sites_verified=0, sites_failed=0, sites_refused=8)


def case_r3_bdnz():
    # bdnz to the next instruction: either way CTR is decremented.
    return want(review3('r3-bdnz', review3_switch(F.rldic(3, 3, 2, 30), ctr_decrement=True, bdnz=True)),
                1, 'INCOMPLETE', sites_verified=0, sites_failed=0, sites_refused=8)


def case_r3_decrementing_bcctr():
    # A bcctr whose BO decrements CTR is no switch dispatch.
    return want(review3('r3-bcctr-dec', review3_switch(F.rldic(3, 3, 2, 30), table1=64, final_bo=16)),
                1, 'INCOMPLETE', sites_verified=0, sites_failed=0, sites_refused=8)


def case_r3_unproven_bctr_dominated():
    # addis ; ld ; mtctr r3 ; bctr: even with the addis before the jump, an
    # unproven jump through CTR opens the whole section.
    return want(flow_case('unproven-dominated', [F.addis(9, 2), F.ld(10, 9), F.mtctr(3), F.bctr()], 0, 4),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_bdzlr_nop():
    # Control: the same sequence with the bdzlr replaced by a nop.
    return want(review3('r3-bdzlr-nop', review3_switch(F.rldic(3, 3, 2, 30), ctr_decrement=True,
                                                     decrement_to_nop=True)),
                0, 'PASS', sites_verified=8, sites_refused=0)


def case_r3_ra_zero_switch():
    # ld r10,LO(0): RA=0 is literal zero, not the addis r0 value.
    return want_detail(review3('r3-ra-zero', review3_switch(F.rldic(3, 3, 2, 30), zero_base=True, table1=64)),
                       1, 'INCOMPLETE', r'RA=0', sites_verified=0, sites_failed=0, sites_refused=8)


def case_r3_r9_base():
    # Control: the same switch with r9 as the base.
    return want(review3('r3-r9-base', review3_switch(F.rldic(3, 3, 2, 30), table1=64)),
                0, 'PASS', sites_verified=8, sites_refused=0)


def case_r3_ra_zero_pair():
    # addis r0,r2,HA ; ld r10,LO(0): never a pair.
    return want_detail(flow_case('ra-zero-pair', [F.addis(0, 2), F.ld(10, 0), F.BLR], 0, 4),
                       1, 'INCOMPLETE', r'RA=0', sites_verified=0, sites_refused=2)


def case_r3_lr_bypass():
    # mtlr r3 ; beqlr: LR may name the use, past the addis.
    return want(flow_case('lr-bypass', [F.mtlr(3), F.bclr(12, 2), F.addis(9, 2), F.ld(10, 9), F.BLR], 8, 12),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_ctr_control():
    # Control: the same through CTR.
    return want(flow_case('ctr-control', [F.mtctr(3), F.bcctr(12, 2), F.addis(9, 2), F.ld(10, 9), F.BLR], 8, 12),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_beqlr_leaf():
    # A conditional return is a computed jump even with LR untouched.
    return want(flow_case('beqlr-leaf', [F.addis(9, 2), F.bclr(12, 2), F.ld(10, 9), F.BLR], 0, 8),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_blr_after_mtlr():
    # mtlr r0 ; blr: LR no longer provably the entry LR.
    return want(flow_case('blr-after-mtlr', [F.addis(9, 2), F.ld(10, 9), F.mtlr(0), F.BLR], 0, 4),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_blr_after_call():
    # bl writes LR; the blr after it is a computed jump.
    return want(flow_case('blr-after-call', [F.addis(30, 2), F.bl(0), F.NOP, F.ld(10, 30), F.BLR], 0, 12,
                          branch=(4, 'R_PPC64_REL24', 'ext', 0), extra=[ext_obj()]),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_bclrl():
    # bclrl: a call through LR, target unknown.
    return want(flow_case('bclrl', [F.addis(9, 2), F.ld(10, 9), F.bclr(20, 0, 1), F.BLR], 0, 4),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_bctrl():
    # mtctr outside a proven switch: the call through CTR has an unknown target.
    return want(flow_case('bctrl', [F.addis(9, 2), F.ld(10, 9), F.mtctr(3), F.bctr() | 1, F.BLR], 0, 4),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r3_plain_return():
    # A plain blr with no LR write on any path is a return.
    return want(flow_case('plain-return', [F.addis(9, 2), F.ld(10, 9), F.BLR], 0, 4),
                0, 'PASS', sites_verified=2, sites_refused=0)


# A non-leaf function as the compilers emit it, with its use of r30 after a call:
#   mflr r0 ; std r0,16(r1) ; stdu r1,-112(r1) ; addis r30,r2,HA ; bl ext ; nop
#   ld r10,LO(r30) ; <epilogue>
# The measured epilogues reload the LR save slot (entry SP + 16, 8 bytes) after
# addi r1,r1,112 (ld r0,16(r1)) or before it (ld r0,128(r1)).

EPILOGUE = [F.addi(1, 1, 112), F.ld(0, 1, 16), F.mtlr(0), F.BLR]
EPILOGUE_BEFORE_POP = [F.ld(0, 1, 128), F.addi(1, 1, 112), F.mtlr(0), F.BLR]


def nonleaf_user(prologue=None, body=(), epilogue=EPILOGUE):
    pro = prologue if prologue is not None else [F.mflr(0), F.std(0, 1, 16), F.stdu(1, 1, -112)]
    words = list(pro) + [F.addis(30, 2), F.bl(0), F.NOP] + list(body) + [F.ld(10, 30)] + list(epilogue)
    ha = 4 * len(pro)
    call = ha + 4
    lo = 4 * (len(pro) + 3 + len(body))
    o = flow_user(words, ha, lo, branch=(call, 'R_PPC64_REL24', 'ext', 0))
    return o


def nonleaf(name, **kw):
    d = fresh(name)
    lk = link(d, [nonleaf_user(**kw), data_obj(), ext_obj()])
    return check(lk.manifest_path)


def case_r4_epilogue():
    # The standard epilogue: ld r0,16(r1) after the frame is popped.
    return want(nonleaf('r4-epilogue'), 0, 'PASS', sites_verified=2, sites_refused=0)


def case_r4_epilogue_before_pop():
    # The other measured form: ld r0,N+16(r1) before addi r1,r1,N.
    return want(nonleaf('r4-epilogue-before-pop', epilogue=EPILOGUE_BEFORE_POP), 0, 'PASS',
                sites_verified=2, sites_refused=0)


def case_r4_backchain():
    # std r3,0(r1) overwrites the back chain; ld r1,0(r1) then restores a
    # stack pointer nothing tracks: the reload is not provably the slot.
    return want(nonleaf('r4-backchain', body=[F.std(3, 1, 0)],
                        epilogue=[F.ld(1, 1, 0), F.ld(0, 1, 16), F.mtlr(0), F.BLR]),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_backchain_control():
    # Control: the same store, the frame popped by addi r1,r1,112.
    return want(nonleaf('r4-backchain-control', body=[F.std(3, 1, 0)]), 0, 'PASS',
                sites_verified=2, sites_refused=0)


def case_r4_partial_slot_stw():
    # stw into the saved slot (entry SP + 16 = 128(r1) in the frame).
    return want(nonleaf('r4-slot-stw', body=[F.stw(3, 1, 128)]), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_partial_slot_stb():
    # stb into the last byte of the saved slot.
    return want(nonleaf('r4-slot-stb', body=[F.stb(3, 1, 135)]), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_store_beside_slot():
    # Control: a store just past the slot (entry SP + 24) leaves it intact.
    return want(nonleaf('r4-slot-beside', body=[F.std(3, 1, 136)]), 0, 'PASS',
                sites_verified=2, sites_refused=0)


def case_r4_divergent_save():
    # The save happens on one path only: the slot facts differ at the merge.
    pro = [F.mflr(0), F.beq(8), F.std(0, 1, 16), F.stdu(1, 1, -112)]
    return want(nonleaf('r4-divergent-save', prologue=pro), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_mtlr_other_register():
    # mtlr from a register that was not reloaded from the slot.
    return want(nonleaf('r4-mtlr-r3', epilogue=[F.addi(1, 1, 112), F.ld(0, 1, 16), F.mtlr(3), F.BLR]),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_save_after_call():
    # mflr r31 ; bl ; std r31,16(r1): the save comes after a call.
    pro = [F.mflr(31), F.stdu(1, 1, -112)]
    body = [F.std(31, 1, 128)]
    return want(nonleaf('r4-save-after-call', prologue=pro, body=body), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_reload_wrong_slot():
    # The reload reads entry SP + 24, not the slot.
    return want(nonleaf('r4-reload-24', epilogue=[F.addi(1, 1, 112), F.ld(0, 1, 24), F.mtlr(0), F.BLR]),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_reload_clobbered():
    # r0 changes between the reload and the mtlr.
    return want(nonleaf('r4-reload-clobbered',
                        epilogue=[F.addi(1, 1, 112), F.ld(0, 1, 16), F.li(0, 0), F.mtlr(0), F.BLR]),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_mflr_after_mtlr():
    # mtlr r3 before the mflr: the saved value is not the entry LR.
    pro = [F.mtlr(3), F.mflr(0), F.std(0, 1, 16), F.stdu(1, 1, -112)]
    return want(nonleaf('r4-mflr-after-mtlr', prologue=pro), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_call_no_restore():
    # A call, then blr with no mtlr: LR holds the call's return address.
    return want(nonleaf('r4-no-restore', epilogue=[F.addi(1, 1, 112), F.BLR]), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_slot_store_elsewhere():
    # A store into the slot on a path that leaves by a tail branch still
    # breaks the slot contract for the whole function.
    o = nonleaf_user(body=[F.beq(12), F.std(3, 1, 128), F.b(0)])
    o.rel('.text.f', 4 * 8, 'R_PPC64_REL24', 'ext', 0)
    d = fresh('r4-slot-elsewhere')
    lk = link(d, [o, data_obj(), ext_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'overlaps the LR save slot',
                       sites_verified=0, sites_refused=2)


def case_r4_named_entry_in_epilogue():
    # A data word names the epilogue's reload: control may arrive there
    # with nothing known.
    o = nonleaf_user()
    o.section('.data.p', bytes(8), flags='WA', align=8)
    o.rel('.data.p', 0, 'R_PPC64_ADDR64', '.text.f', 4 * 10)
    d = fresh('r4-named-epilogue')
    lk = link(d, [o, data_obj(), ext_obj()])
    return want(check(lk.manifest_path), 1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_indexed_stack_store():
    # stdx r3,r1,r4: an r1-indexed store at an unknown offset.
    return want_detail(nonleaf('r4-stdx', body=[F.x_form(149, 3, 1, 4)]), 1, 'INCOMPLETE',
                       r'may write the LR save slot', sites_verified=0, sites_refused=2)


def case_r4_non_copy_save():
    # std r3,16(r1): the slot holds r3, not the mflr copy.
    pro = [F.mflr(0), F.std(3, 1, 16), F.stdu(1, 1, -112)]
    return want(nonleaf('r4-non-copy-save', prologue=pro), 1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_backchain_stale_offset():
    # After ld r1,0(r1) the frame offset 128 no longer names the slot.
    return want(nonleaf('r4-backchain-stale', epilogue=[F.ld(1, 1, 0), F.ld(0, 1, 128), F.mtlr(0), F.BLR]),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_store_into_slot_start():
    # std r3,124(r1) covers entry SP + 12 to + 20: it reaches the slot by its width.
    return want(nonleaf('r4-slot-start', body=[F.std(3, 1, 124)]), 1, 'INCOMPLETE',
                sites_verified=0, sites_refused=2)


def case_r4_stfd_into_slot_start():
    # stfd f1,124(r1): an 8-byte D-form store from entry SP + 12 into the slot.
    stfd = (54 << 26) | (1 << 21) | (1 << 16) | 124
    return want(nonleaf('r4-stfd-slot-start', body=[stfd]), 1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def call_on_one_path(order):
    """A call on one of two paths that merge before blr (no mtlr): LR differs at the merge."""
    if order == 'fall':
        words = [F.addis(30, 2), F.beq(12), F.bl(0), F.NOP, F.ld(10, 30), F.BLR]
        call, lo = 8, 16
    else:
        words = [F.addis(30, 2), F.beq(12), F.b(12), F.bl(0), F.NOP, F.ld(10, 30), F.BLR]
        call, lo = 12, 20
    o = flow_user(words, 0, lo, branch=(call, 'R_PPC64_REL24', 'ext', 0))
    d = fresh('r4-call-one-path-' + order)
    lk = link(d, [o, data_obj(), ext_obj()])
    return want(check(lk.manifest_path), 1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_r4_call_on_fallthrough():
    return call_on_one_path('fall')


def case_r4_call_on_taken():
    return call_on_one_path('taken')


def case_jump_table_last():
    # The same switch with its table at the section's end.
    return proven('switch-table-last', table_last=True)


def case_jump_table_unknown():
    # The table address names no table.
    return unproven('switch-unnamed', table_named=False)


def case_jump_table_unbounded():
    # No range check: the table has no proven length.
    return unproven('switch-unbounded', check=None)


def case_jump_table_index_changed():
    # The checked register changes before it indexes the table.
    return unproven('switch-index-changed', clobber_index=True)


def case_jump_table_past_section():
    # The range check allows more entries than the section holds.
    return unproven('switch-past-end', check=('cmplwi', 3, 40))


def case_jump_table_last_past_end():
    # Table at the end, range check one entry past it.
    return unproven('switch-last-past-end', table_last=True, check=('cmplwi', 3, 2))


def case_jump_table_into_dispatch():
    # A table entry that jumps back into the dispatch sequence breaks its proof.
    return unproven('switch-into-dispatch', case0_at='mtctr')


def case_jump_table_into_head():
    # A table entry back to the dispatch's first instruction adds a path
    # the proof did not run.
    return unproven('switch-into-head', reenter=(4, 1), case0_at='head')


def case_jump_table_wrong_base():
    # The entry is added to another table address: nothing is proven.
    return unproven('switch-wrong-base', refused=8, wrong_base=True)


def case_jump_table_blt():
    # blt instead of bgt: the fall-through does not bound the index.
    return unproven('switch-blt', bc=(12, 0))


def case_jump_table_ble():
    # ble (bc 4,gt) falls through when the index is out of range.
    return unproven('switch-ble', bc=(4, 1))


def case_jump_table_target_skips_ha():
    # A proven table entry lands on case 0's use, past its addis.
    return want_detail(switch_case('switch-skip-ha', case0_at='lo'), 1, 'INCOMPLETE',
                       r'r12', sites_verified=4, sites_refused=2)


def case_jump_table_entry_inside():
    # Another entry point inside the dispatch sequence: nothing is proven.
    return unproven('switch-entry-inside', entry_at_mtctr=True)


# -- the second review's reproductions ----------------------------------------

def ext_obj():
    o = from_obj('ext.o')
    o.section('.text.ext', F.code(F.NOP, F.BLR))
    o.sym('ext', '.text.ext', 0, bind='GLOBAL', stype='FUNC')
    return o


def flow_user(words, ha, lo, branch=None, name='f.o'):
    """f: words; TOC16_HA at ha, TOC16_LO_DS at lo (both .LC0 -> g_a)."""
    o = from_obj(name)
    o.section('.text.f', F.code(*words))
    o.sym('f', '.text.f', 0, bind='GLOBAL', stype='FUNC')
    o.section('.toc', bytes(8), flags='WA', align=8)
    o.sym('.LC0', '.toc', 0)
    o.sym('g_a', 'UND', bind='GLOBAL')
    o.rel('.toc', 0, 'R_PPC64_ADDR64', 'g_a')
    o.rel('.text.f', ha + 2, 'R_PPC64_TOC16_HA', '.LC0')
    o.rel('.text.f', lo + 2, 'R_PPC64_TOC16_LO_DS', '.LC0')
    if branch:
        off, rtype, sym, addend = branch
        if sym == 'ext':
            o.sym('ext', 'UND', bind='GLOBAL')
        o.rel('.text.f', off, rtype, sym, addend)
    return o


def flow_case(name, *args, extra=(), **kw):
    d = fresh(name)
    lk = link(d, [flow_user(*args, **kw), data_obj()] + list(extra))
    return check(lk.manifest_path)


def case_reloc_branch_bypass():
    # beq carries REL14 to the use, past the addis: the taken path reaches
    # the use with r9 unknown.
    return want(flow_case('reloc-bypass', [F.beq(0), F.addis(9, 2), F.NOP, F.ld(10, 9), F.BLR], 4, 12,
                          branch=(0, 'R_PPC64_REL14', '.text.f', 12)),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_encoded_branch_bypass():
    # The same branch with its displacement encoded (no relocation).
    return want(flow_case('encoded-bypass', [F.beq(12), F.addis(9, 2), F.NOP, F.ld(10, 9), F.BLR], 4, 12),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_reloc_branch_around():
    # A REL14 branch that skips past the use: no path bypasses the addis.
    return want(flow_case('reloc-around', [F.addis(9, 2), F.beq(0), F.ld(10, 9), F.BLR], 0, 8,
                          branch=(4, 'R_PPC64_REL14', '.text.f', 12)),
                0, 'PASS', sites_verified=2, sites_refused=0)


def case_reloc_branch_external():
    # A conditional branch to another function keeps its fall-through path.
    return want(flow_case('reloc-external', [F.addis(9, 2), F.beq(0), F.ld(10, 9), F.BLR], 0, 8,
                          branch=(4, 'R_PPC64_REL14', 'ext', 0), extra=[ext_obj()]),
                0, 'PASS', sites_verified=2, sites_refused=0)


def case_reloc_call_target_entry():
    # bl with REL24 to the use: a call target is entered with unknown
    # registers.  r30 survives the call, and the function leaves by a tail
    # branch (a blr after the bl would be a computed jump on its own).
    o = flow_user([F.addis(30, 2), F.bl(0), F.NOP, F.ld(10, 30), F.b(0)], 0, 12,
                  branch=(4, 'R_PPC64_REL24', '.text.f', 12))
    o.sym('ext', 'UND', bind='GLOBAL')
    o.rel('.text.f', 16, 'R_PPC64_REL24', 'ext', 0)
    d = fresh('reloc-call')
    lk = link(d, [o, data_obj(), ext_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'entry point', sites_verified=0, sites_refused=2)


def case_reloc_branch_offset_outside():
    # A branch to another section's symbol plus an addend may land anywhere.
    return want_detail(flow_case('reloc-addend-out', [F.addis(9, 2), F.beq(0), F.ld(10, 9), F.BLR], 0, 8,
                                 branch=(4, 'R_PPC64_REL14', 'ext', 4), extra=[ext_obj()]),
                       1, 'INCOMPLETE', r'outside', sites_verified=0, sites_refused=2)


def descriptor_call_user(into_use):
    """
    f: addis r30; bl .opd+8 (REL24); nop; ld r10,LO(r30); b ext (REL24)
    .opd+8 holds the code address .text.f+12 (the use) or g in .text.g;
    r30 is non-volatile, so it survives the call.  The call names the
    descriptor as section+addend, as compilers do for local functions.
    The tail branch leaves the section: a blr after the bl would be a
    computed jump.
    """
    o = flow_user([F.addis(30, 2), F.bl(0), F.NOP, F.ld(10, 30), F.b(0)], 0, 12,
                  branch=(16, 'R_PPC64_REL24', 'ext', 0))
    o.section('.text.g', F.code(F.BLR))
    o.sym('g', '.text.g', 0, stype='FUNC')
    o.section('.opd', bytes(16), flags='WA', align=8)
    o.rel('.opd', 8, 'R_PPC64_ADDR32', '.text.f' if into_use else 'g', 12 if into_use else 0)
    o.rel('.text.f', 4, 'R_PPC64_REL24', '.opd', 8)
    return o


def case_descriptor_call_into_use():
    # A call through a local descriptor whose code address is the use.
    d = fresh('opd-call-into')
    lk = link(d, [descriptor_call_user(True), data_obj(), ext_obj()])
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'entry point', sites_verified=0, sites_refused=2)


def case_descriptor_call_elsewhere():
    # A call through a local descriptor to another section returns to the nop.
    d = fresh('opd-call-out')
    lk = link(d, [descriptor_call_user(False), data_obj(), ext_obj()])
    return want(check(lk.manifest_path), 0, 'PASS', sites_verified=2, sites_refused=0)


def case_unknown_ctr_bypass():
    # mtctr r3; beqctr: CTR may name the use, past the addis.
    return want(flow_case('ctr-bypass', [F.mtctr(3), F.bcctr(12, 2), F.addis(9, 2), F.ld(10, 9), F.BLR], 8, 12),
                1, 'INCOMPLETE', sites_verified=0, sites_refused=2)


def case_lswi_clobber():
    # lswi r8,r3,8 loads r8 and r9.
    return want_detail(flow_case('lswi', [F.addis(9, 2), F.lswi(8, 3, 8), F.ld(10, 9), F.BLR], 0, 8),
                       1, 'INCOMPLETE', r'r9 may change at \+0x4', sites_verified=0, sites_refused=2)


def case_lswi_short():
    # lswi r8,r3,4 loads r8 only: r9 survives.
    return want(flow_case('lswi-short', [F.addis(9, 2), F.lswi(8, 3, 4), F.ld(10, 9), F.BLR], 0, 8),
                0, 'PASS', sites_verified=2, sites_refused=0)


def case_lswx_clobber():
    # lswx's register count comes from XER: any register may change.
    # The function leaves by a tail branch, so no return proof is involved.
    return want_detail(flow_case('lswx', [F.addis(9, 2), F.lswx(30, 3, 4), F.ld(10, 9), F.b(0)], 0, 8,
                                 branch=(12, 'R_PPC64_REL24', 'ext', 0), extra=[ext_obj()]),
                       1, 'INCOMPLETE', r'r9 may change at \+0x4', sites_verified=0, sites_refused=2)


def case_stfdux_clobber():
    # stfdux f8,r9,r3 updates r9.
    return want_detail(flow_case('stfdux', [F.addis(9, 2), F.stfdux(8, 9, 3), F.ld(10, 9), F.BLR], 0, 8),
                       1, 'INCOMPLETE', r'r9 may change at \+0x4', sites_verified=0, sites_refused=2)


def case_unknown_x_form():
    # An X-form the tool does not know is a possible write of any register.
    # The function leaves by a tail branch, so no return proof is involved.
    return want_detail(flow_case('unknown-x', [F.addis(9, 2), F.x_form(1000, 8, 3, 4), F.ld(10, 9), F.b(0)], 0, 8,
                                 branch=(12, 'R_PPC64_REL24', 'ext', 0), extra=[ext_obj()]),
                       1, 'INCOMPLETE', r'r9 may change at \+0x4', sites_verified=0, sites_refused=2)


def gpr_writes_fn():
    spec = importlib.util.spec_from_file_location('toc_linked_tool', TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.gpr_writes


def case_gpr_writes_table():
    gw = gpr_writes_fn()
    rows = [
        ('lswi r8,r3,8', F.lswi(8, 3, 8), {8, 9}),
        ('lswi r30,r3,12 wraps', F.lswi(30, 3, 12), {30, 31, 0}),
        ('lswi r8,r3,0 is 32 bytes', F.lswi(8, 3, 0), set(range(8, 16))),
        ('lswi r8,r3,5 loads part of r9', F.lswi(8, 3, 5), {8, 9}),
        ('lswx', F.lswx(8, 3, 4), None),
        ('stfdux', F.stfdux(8, 9, 3), {9}),
        ('lwax', F.lwax(9, 10, 3), {9}),
        ('add', F.add(9, 9, 10), {9}),
        ('addo (OE)', F.add(9, 9, 10) | (1 << 10), {9}),
        ('sldi', F.sldi(3, 4, 2), {3}),
        ('mtctr', F.mtctr(9), set()),
        ('unknown X-form', F.x_form(1000, 8, 3, 4), None),
        ('unknown primary opcode', 1 << 26, None),
        ('ld with reserved DS bits', F.ld(9, 3) | 3, None),
        ('std with reserved DS bits', (62 << 26) | (9 << 21) | (3 << 16) | 2, None),
        ('unknown XL-form', (19 << 26) | (77 << 1), None),
    ]
    bad = []
    for name, w, expect in rows:
        got = gw(w)
        if got != expect:
            bad.append(f"{name} (0x{w & 0xffffffff:08x}): got {got}, expected {expect}")
    return (not bad), '; '.join(bad) or f"{len(rows)} forms"


NEGATIVE_CASES = [
    ('review: wrong displacement while output relocations agree fails', case_wrong_displacement_relocs_agree),
    ('review: stale or shifted map placement is refused', case_stale_map_placement),
    ('review: missing map placement is refused', case_missing_map_placement),
    ('review: a discarded section is counted, not refused', case_discarded_section),
    ('review: duplicate local names resolve per object', case_duplicate_local_names),
    ('review: duplicate local names, wrong object entry fails', case_duplicate_local_names_swapped),
    ('review: duplicate section names in one object are refused', case_duplicate_section_names),
    ('review: an archive member is checked like an object', case_archive_member_green),
    ('review: archive members that share a name are refused', case_archive_member_ambiguity),
    ('review: 4-byte load of an 8-byte entry at +4', case_low_word_of_entry),
    ('review: 4-byte load of the wrong word fails', case_low_word_of_entry_wrong),
    ('review: signed LO carry at 0x8000', case_lo_carry),
    ('review: dropped HA carry fails', case_lo_carry_dropped),
    ('review: changed DS-form bits are refused', case_ds_form_changed),
    ('review: correct nop rewrite of the addis', case_nop_rewrite),
    ('review: nop rewrite that keeps the base register fails', case_nop_rewrite_wrong),
    ('review: correct addi rewrite of a .toc load', case_addi_rewrite),
    ('review: wrong addi rewrite fails', case_addi_rewrite_wrong),
    ('review: a read outside file-backed sections fails', case_read_outside_file),
    ('review: malformed (truncated) executable is bad input', case_truncated_elf),
    ('review: unpaired TOC16_HA is accounted as refused', case_unpaired_ha),
    ('review: one HA shared by two uses is consumed once', case_shared_ha),
    ('review: an unsupported TOC relocation is refused, not skipped', case_unsupported_relocation),
    ('review: an unknown instruction form is refused', case_unknown_form),
    ('review: a descriptor with another TOC is bad input', case_multiple_toc),
    ('review: a descriptor on the one TOC', case_single_toc_descriptor),
    ('review: a linker stub section is bad input', case_stub_section),
    ('review: zero coverage is INCOMPLETE', case_zero_coverage),
    ('review: stripped executable is bad input', case_stripped_elf),
    ('review: missing map is bad input', case_missing_map),
    ('review: an input that changed after capture is bad input', case_wrong_object_set),
    ('review: a trace naming files outside the manifest is bad input', case_trace_names_other_file),
    ('flow: computed jump through a proven switch table', case_jump_table),
    ('flow: a table address in a 4-byte .toc entry', case_jump_table_toc32),
    ('flow: cmpldi bounds sldi of the whole register', case_jump_table_cmpldi_sldi),
    ('flow: low-byte check bounds a low-byte scale', case_jump_table_low_byte),
    ('flow: a twi trap bounds the index', case_jump_table_trap_bound),
    ('flow: a second checked path re-enters the dispatch', case_jump_table_reentered),
    ('flow: an unchecked path re-enters the dispatch', case_jump_table_reentered_unbounded),
    ('flow: a 32-bit check does not bound sldi', case_jump_table_sldi_after_32bit_check),
    ('flow: a byte check does not bound a 32-bit scale', case_jump_table_byte_check_wider_scale),
    ('flow: a reloaded index is a new value', case_jump_table_reload),
    ('flow: a case that falls into a second dispatch', case_jump_table_nested),
    ('review 3: rldic with a wrapping mask is refused', case_r3_rldic_wrap),
    ('review 3: rlwinm with a wrapping mask is refused', case_r3_rlwinm_wrap),
    ('review 3: rldic 2,30 control', case_r3_rldic_normal),
    ('review 3: rlwinm 2,0,29 control', case_r3_rlwinm_normal),
    ('review 3: bdzlr in the dispatch is refused', case_r3_bdzlr),
    ('review 3: bdnz in the dispatch is refused', case_r3_bdnz),
    ('review 3: a decrementing bcctr is no dispatch', case_r3_decrementing_bcctr),
    ('review 3: an unproven bctr opens the section', case_r3_unproven_bctr_dominated),
    ('review 3: bdzlr replaced by nop control', case_r3_bdzlr_nop),
    ('review 3: a table address loaded with RA=0 is refused', case_r3_ra_zero_switch),
    ('review 3: r9 base control', case_r3_r9_base),
    ('review 3: an LO on RA=0 never pairs', case_r3_ra_zero_pair),
    ('review 3: mtlr + beqlr past the addis is refused', case_r3_lr_bypass),
    ('review 3: mtctr + beqctr control', case_r3_ctr_control),
    ('review 3: a conditional return is a computed jump', case_r3_beqlr_leaf),
    ('review 3: blr after mtlr is a computed jump', case_r3_blr_after_mtlr),
    ('review 3: blr after a call is a computed jump', case_r3_blr_after_call),
    ('review 3: bclrl is a computed call', case_r3_bclrl),
    ('review 3: bctrl outside a switch is a computed call', case_r3_bctrl),
    ('review 3: a plain blr with LR untouched returns', case_r3_plain_return),
    ('review 4: the standard non-leaf epilogue returns', case_r4_epilogue),
    ('review 4: the reload-before-pop epilogue returns', case_r4_epilogue_before_pop),
    ('review 4: a back-chain restore is refused', case_r4_backchain),
    ('review 4: back-chain store with addi restore control', case_r4_backchain_control),
    ('review 4: stw into the LR save slot is refused', case_r4_partial_slot_stw),
    ('review 4: stb into the LR save slot is refused', case_r4_partial_slot_stb),
    ('review 4: a store beside the slot control', case_r4_store_beside_slot),
    ('review 4: a save on one path only is refused', case_r4_divergent_save),
    ('review 4: mtlr from a non-slot register is refused', case_r4_mtlr_other_register),
    ('review 4: a save after a call is refused', case_r4_save_after_call),
    ('review 4: a reload from the wrong offset is refused', case_r4_reload_wrong_slot),
    ('review 4: a reload changed before mtlr is refused', case_r4_reload_clobbered),
    ('review 4: mflr after mtlr is not the entry LR', case_r4_mflr_after_mtlr),
    ('review 4: blr after a call with no restore is refused', case_r4_call_no_restore),
    ('review 4: a slot store on another path is refused', case_r4_slot_store_elsewhere),
    ('review 4: a named address in the epilogue is refused', case_r4_named_entry_in_epilogue),
    ('review 4: an r1-indexed store is refused', case_r4_indexed_stack_store),
    ('review 4: a save of another register is refused', case_r4_non_copy_save),
    ('review 4: a stale frame offset after a back-chain restore is refused', case_r4_backchain_stale_offset),
    ('review 4: a store reaching the slot by its width is refused', case_r4_store_into_slot_start),
    ('review 4: an stfd reaching the slot by its width is refused', case_r4_stfd_into_slot_start),
    ('review 4: a call on the fall-through path only is refused', case_r4_call_on_fallthrough),
    ('review 4: a call on the taken path only is refused', case_r4_call_on_taken),
    ('flow: computed jump with no named table reaches anything', case_jump_table_unknown),
    ('flow: switch without a range check reaches anything', case_jump_table_unbounded),
    ('flow: switch whose checked index changes reaches anything', case_jump_table_index_changed),
    ('flow: switch range past the section reaches anything', case_jump_table_past_section),
    ('flow: proven switch with its table at the section end', case_jump_table_last),
    ('flow: table at the end, range one past it', case_jump_table_last_past_end),
    ('flow: table entry back into the dispatch breaks the proof', case_jump_table_into_dispatch),
    ('flow: table entry back to the dispatch head breaks the proof', case_jump_table_into_head),
    ('flow: entry added to another table address proves nothing', case_jump_table_wrong_base),
    ('flow: blt does not bound the index', case_jump_table_blt),
    ('flow: ble falls through out of range', case_jump_table_ble),
    ('flow: proven table entry that skips an addis is refused', case_jump_table_target_skips_ha),
    ('flow: entry point inside the dispatch proves nothing', case_jump_table_entry_inside),
    ('review 2: REL14 branch past the addis is refused', case_reloc_branch_bypass),
    ('review 2: encoded branch past the addis is refused (control)', case_encoded_branch_bypass),
    ('review 2: REL14 branch past the use keeps the pair', case_reloc_branch_around),
    ('review 2: branch to another function keeps the fall-through', case_reloc_branch_external),
    ('review 2: a call into the section enters with unknown registers', case_reloc_call_target_entry),
    ('review 2: branch to another section plus an addend is refused', case_reloc_branch_offset_outside),
    ('review 2: a call through a descriptor to the use enters it', case_descriptor_call_into_use),
    ('review 2: a call through a descriptor to another section returns', case_descriptor_call_elsewhere),
    ('review 2: unknown CTR jump past the addis is refused', case_unknown_ctr_bypass),
    ('review 2: lswi that loads the base register is refused', case_lswi_clobber),
    ('review 2: lswi that stops short of the base register', case_lswi_short),
    ('review 2: lswx is refused', case_lswx_clobber),
    ('review 2: stfdux updating the base register is refused', case_stfdux_clobber),
    ('review 2: unknown X-form is refused', case_unknown_x_form),
    ('review 2: GPR write table', case_gpr_writes_table),
]


CASES = [
    ('defect: merged string, local symbol with a negative addend', case_merged_string_negative_addend),
    ('defect: merged string, negative addend, wrong displacement fails', case_merged_string_negative_addend_wrong),
    ('defect: section name longer than readelf prints', case_long_section_name),
    ('defect: addis reaches its use only through a branch', case_branch_around_clobber),
    ('defect: a clobber on a path to the use is refused', case_branch_bypass_refused),
    ('defect: a use that is also an entry point is refused', case_branch_entry_refused),
    ('green: medium and small model loads of .toc entries', case_green),
] + NEGATIVE_CASES


def main():
    global READELF, WORK
    READELF = find_readelf()
    if not READELF:
        print("toc-linked-check-test: FAIL: no readelf (set PPU_READELF)")
        return 1
    only = sys.argv[1:]
    WORK = tempfile.mkdtemp(prefix='toc-linked-')
    keep = os.environ.get('TOC_LINKED_KEEP')
    passed = failed = 0
    try:
        for name, fn in CASES:
            if only and not any(o in name for o in only):
                continue
            try:
                ok, note = fn()
            except Exception as ex:  # a harness error is a failure, never a skip
                ok, note = False, f"harness error: {ex!r}"
            if ok:
                passed += 1
                print(f"ok   {name}" + (f" ({note})" if note else ''))
            else:
                failed += 1
                print(f"FAIL {name}\n{note}")
    finally:
        if keep:
            print(f"work kept in {WORK}")
        else:
            shutil.rmtree(WORK, ignore_errors=True)
    print(f"toc-linked-check-test: {passed} passed, {failed} failed")
    if passed == 0:
        return 1
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
