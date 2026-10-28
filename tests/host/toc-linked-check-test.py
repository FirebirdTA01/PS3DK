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
    return want_detail(check(lk.manifest_path), 1, 'INCOMPLETE', r'has no paired access',
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
]


CASES = [
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
