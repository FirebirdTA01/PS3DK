#!/usr/bin/env python3
"""P3.3 R4b archive contract (design r2 delta section 3; review 22:15 CHANGES).  The EXPECTED member set and external
import set are fixed here, from the reviewed contract; sdk/libps3dkprof/ext-members.mk is parsed and compared AGAINST
them, so editing that list cannot change the expectation.  Names are kept verbatim as nm prints them (".TOC."
included).  Any difference is a FAIL naming it and stops the checkpoint for review.

Rules: every nm / readelf record is classified or the check fails; an undefined symbol is discharged only by an
externally visible (GLOBAL or WEAK) definition in some member, never by a local one; weak undefined references are
judged explicitly (the expected weak set is empty); each member has exactly one ELF header with each field once, and
exactly one Tag_GNU_Power_CellOS_Data_Model attribute, which must be ILP32 (the e_flags are the same for ILP32 and
LP64, so they cannot tell the ABIs apart); the reference archive's headers must themselves be complete and uniform.

usage: r4b_archive.py --ar AR --nm NM --readelf READELF --archive LIB.a --members-mk ext-members.mk
                      --ref-archive SHIPPED_ILP32.a          (exit 0 iff every check passes)
The judging functions take tool output text, so r4b_selftest.py drives them with synthetic inputs."""
import argparse
import re
import subprocess
import sys

MEMBERS = ('prof_core.o', 'prof_drain.o', 'pal_lv2.o', 'ext_runtime.o', 'psp_ppu.o', 'psp_elf.o', 'gpu_runtime.o',
           'prg.o', 'gpu_port_gcm.o')
# measured from the candidate libps3dkprof.a (nm, raw spelling) + the R3 GCM accessors + strcpy (psp_ppu.c lane name)
IMPORTS_R4B_R2 = ('.TOC.', '__errno', 'clock_gettime', 'close', 'free', 'memalign', 'memcmp', 'memcpy', 'memset',
                  'nanosleep', 'open', 'pthread_attr_destroy', 'pthread_attr_init', 'pthread_attr_setstacksize',
                  'pthread_create', 'pthread_join', 'pthread_mutex_destroy', 'pthread_mutex_init',
                  'pthread_mutex_lock', 'pthread_mutex_unlock', 'snprintf', 'strlen', 'sysThreadGetId', 'write',
                  'gcmGetLabelAddress', 'gcmGetReportDataAddressLocation', 'rsxFlushBuffer', 'strcpy')
# Contract amendment (review 23:01, exactly these three): the 10-31 reference archive predates pal_lv2.c's pal_read
# (read), pal_rewind (lseek) and pal_sync (fsync), so the r2 baseline above was stale; target attempt 2 (r4b-t3)
# failed on exactly them.  Any further unexpected import still stops the checkpoint.
IMPORTS = IMPORTS_R4B_R2 + ('fsync', 'lseek', 'read')
WEAK_IMPORTS = ()
HEADER = {'Class': 'ELF64', 'Data': "2's complement, big endian", 'Machine': 'PowerPC64'}
REF_FLAGS = '0x0'  # measured on the shipped ILP32 (and LP64) objects
MK_LINE = re.compile(r'^EXT_MEMBERS := ([a-z0-9_]+(?: [a-z0-9_]+)*)$')
NM_UND = re.compile(r'^\s+([Uwv])\s+(\S+)$')
NM_DEF = re.compile(r'^[0-9a-f]{8,16}\s+([A-Za-z])\s+(\S+)$')
NM_MEMBER = re.compile(r'^(\S+\.o):$')
GLOBAL_DEF = set('TDBRCVWAG' 'S')  # uppercase = externally visible; V/W weak definitions
LOCAL_DEF = set('tdbrgsa')


class Bad(Exception):
    pass


def judge_members_mk(text):
    lines = [l for l in text.splitlines() if l.startswith('EXT_MEMBERS')]
    if len(lines) != 1:
        return [f'ext-members.mk has {len(lines)} EXT_MEMBERS lines (want exactly 1)']
    m = MK_LINE.match(lines[0])
    if not m:
        return ['ext-members.mk EXT_MEMBERS line malformed']
    got = [n + '.o' for n in m.group(1).split()]
    if got != list(MEMBERS):
        return [f'member list differs from the contract: {got}']
    return []


def judge_members(ar_t_text):
    got = [l.strip() for l in ar_t_text.splitlines() if l.strip()]
    errs = [f'duplicate member {n}' for n in sorted(set(got)) if got.count(n) > 1]
    errs += [f'missing member {n}' for n in MEMBERS if n not in got]
    errs += [f'extra member {n}' for n in got if n not in MEMBERS]
    return errs


def parse_readelf_h(text):
    """readelf -h of an archive -> {member: {field: [values]}}, every record classified"""
    out, cur = {}, None
    for l in text.splitlines():
        s = l.strip()
        m = re.match(r'^File: .*\(([^)]+)\)$', s)
        if m:
            if m.group(1) in out:
                raise Bad(f'duplicate header for {m.group(1)}')
            cur = out[m.group(1)] = {}
            continue
        m = re.match(r'^([A-Za-z][A-Za-z0-9 ()/.-]*):\s+(.*?)\s*$', s)
        if m and cur is not None:
            cur.setdefault(m.group(1), []).append(m.group(2))
        elif s and s != 'ELF Header:' and not s.startswith('Magic:'):
            raise Bad(f'unrecognised readelf -h record {s!r}')
    return out


def judge_headers(hdr, ref_hdr):
    errs = []
    if not ref_hdr:
        errs.append('reference archive: no headers')
    for n, h in ref_hdr.items():
        if h.get('Flags') != [REF_FLAGS]:
            errs.append(f'reference {n}: Flags {h.get("Flags")} (want exactly [{REF_FLAGS!r}])')
        for k, v in HEADER.items():
            if h.get(k) != [v]:
                errs.append(f'reference {n}: {k} {h.get(k)}')
    for n in hdr:
        if n not in MEMBERS:
            errs.append(f'header for a non-member {n}')
    for n in MEMBERS:
        h = hdr.get(n)
        if h is None:
            errs.append(f'no ELF header for {n}')
            continue
        for k, v in list(HEADER.items()) + [('Flags', REF_FLAGS)]:
            if h.get(k) != [v]:
                errs.append(f'{n}: {k} {h.get(k)} (want exactly [{v!r}])')
    return errs


def judge_attributes(text):
    """readelf -A of the archive: each member exactly one data-model tag, ILP32"""
    tags, cur = {}, None
    for l in text.splitlines():
        s = l.strip()
        m = re.match(r'^File: .*\(([^)]+)\)$', s)
        if m:
            cur = m.group(1)
            if cur in tags:
                raise Bad(f'duplicate attribute block for {cur}')
            tags[cur] = []
        elif (m := re.match(r'^Tag_GNU_Power_CellOS_Data_Model:\s+(\S+)$', s)) and cur:
            tags[cur].append(m.group(1))
        elif s and s not in ('Attribute Section: gnu', 'File Attributes') and not s.startswith('Tag_'):
            raise Bad(f'unrecognised readelf -A record {s!r}')
    errs = []
    for n in MEMBERS:
        if tags.get(n) != ['ILP32']:
            errs.append(f'{n}: data model {tags.get(n)} (want exactly [ILP32])')
    errs += [f'attributes for a non-member {n}' for n in tags if n not in MEMBERS]
    return errs


def judge_imports(nm_text):
    """nm (default format) of the archive.  Returns (errors, external undefined, weak undefined)."""
    und, weak, gdef = set(), set(), set()
    errs = []
    blocks = []
    for l in nm_text.splitlines():
        if not l.strip():
            continue
        mm = NM_MEMBER.match(l)
        if mm:
            blocks.append(mm.group(1))
            continue
        if not blocks:
            errs.append(f'nm record before any member block {l.strip()!r}')
            continue
        m = NM_UND.match(l)
        if m:
            (weak if m.group(1) in 'wv' else und).add(m.group(2))
            continue
        m = NM_DEF.match(l)
        if m and m.group(1) in GLOBAL_DEF:
            gdef.add(m.group(2))
            continue
        if m and m.group(1) in LOCAL_DEF:
            continue  # a local definition discharges nothing outside its member
        errs.append(f'unclassified nm record {l.strip()!r}')
    if sorted(blocks) != sorted(MEMBERS) or len(blocks) != len(MEMBERS):
        errs.append(f'nm member blocks {blocks} (want exactly one block for each of the {len(MEMBERS)} members)')
    ext = und - gdef
    wext = weak - gdef
    errs += [f'unexpected import {n}' for n in sorted(ext - set(IMPORTS))]
    errs += [f'expected import absent {n}' for n in sorted(set(IMPORTS) - ext)]
    errs += [f'unexpected weak import {n}' for n in sorted(wext - set(WEAK_IMPORTS))]
    return errs, sorted(ext), sorted(wext)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise Bad(f'tool failure {cmd[0]} rc {r.returncode}: {r.stderr[:300]}')
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    for a in ('--ar', '--nm', '--readelf', '--archive', '--members-mk', '--ref-archive'):
        ap.add_argument(a, required=True)
    a = ap.parse_args()
    errs = []
    try:
        errs += judge_members_mk(open(a.members_mk, encoding='ascii').read())
        errs += judge_members(run([a.ar, 't', a.archive]))
        errs += judge_headers(parse_readelf_h(run([a.readelf, '-h', a.archive])),
                              parse_readelf_h(run([a.readelf, '-h', a.ref_archive])))
        errs += judge_attributes(run([a.readelf, '-A', a.archive]))
        e, ext, wext = judge_imports(run([a.nm, a.archive]))
        errs += e
        print('external undefined (verbatim): ' + ' '.join(ext))
        print('weak undefined: ' + (' '.join(wext) or '(none)'))
    except Bad as b:
        errs.append(str(b))
    for x in errs:
        print('FAIL ' + x)
    print('R4B ARCHIVE ' + ('PASS' if not errs else 'FAIL'))
    return 0 if not errs else 1


if __name__ == '__main__':
    sys.exit(main())
