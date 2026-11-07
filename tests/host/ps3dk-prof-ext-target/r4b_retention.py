#!/usr/bin/env python3
"""P3.3 R4b link retention (design r2 delta section 3).  A member counts as LINKED when at least one of its
externally visible definitions (from nm of the archive, strict records, exactly one block per contract member) is a
symbol of the image (readelf -sW, strict inventory).  --expect all: every contract member linked (the positive
fixture); --expect none: no member linked (the negative fixture).  Retention is object-granular (R3: --gc-sections
keeps every function of a pulled-in ELFv1 object; observed in ILP32, and the LP64 run checks the same rule), so a
member's presence is what this proves, nothing finer.

usage: r4b_retention.py --archive-nm NM.txt --image-syms READELF_S.txt --expect all|none
A witness is an image symbol that is GLOBAL or WEAK and DEFINED in a numeric nonzero section (never UND, LOCAL, ABS
or COMMON)."""
import argparse
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import r4b_archive as A  # noqa: E402
import r4b_image as I  # noqa: E402


def member_defs(nm_text):
    defs, cur, errs = {}, None, []
    for l in nm_text.splitlines():
        if not l.strip():
            continue
        m = A.NM_MEMBER.match(l)
        if m:
            cur = m.group(1)
            if cur in defs:
                errs.append(f'duplicate nm block {cur}')
            defs[cur] = set()
            continue
        if cur is None:
            errs.append(f'nm record before any member block {l.strip()!r}')
            continue
        if A.NM_UND.match(l):
            continue
        d = A.NM_DEF.match(l)
        if d and d.group(1) in A.GLOBAL_DEF:
            defs[cur].add(d.group(2))
        elif not (d and d.group(1) in A.LOCAL_DEF):
            errs.append(f'unclassified nm record {l.strip()!r}')
    if sorted(defs) != sorted(A.MEMBERS):
        errs.append(f'nm blocks {sorted(defs)} differ from the contract members')
    return defs, errs


def judge(nm_text, syms_text, expect):
    defs, errs = member_defs(nm_text)
    try:
        # a witness must be an externally visible DEFINITION in the image: GLOBAL or WEAK, in a numeric nonzero
        # section (UND, LOCAL, ABS and COMMON never establish that a member's code/data was linked; review 23:35)
        names = {s['name'] for s in I.parse_symbols(syms_text)
                 if s['bind'] in ('GLOBAL', 'WEAK') and s['ndx'].isdigit() and int(s['ndx']) != 0}
    except I.Unmet as u:
        return [f'image symbols UNMET: {u}'], {}
    linked = {m: sorted(defs.get(m, set()) & names) for m in A.MEMBERS}
    for m in A.MEMBERS:
        if not defs.get(m):
            errs.append(f'{m}: no externally visible definition to witness it')
        elif expect == 'all' and not linked[m]:
            errs.append(f'{m}: NOT linked into the positive fixture')
        elif expect == 'none' and linked[m]:
            errs.append(f'{m}: linked into the negative fixture ({linked[m][:3]})')
    return errs, linked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--archive-nm', required=True)
    ap.add_argument('--image-syms', required=True)
    ap.add_argument('--expect', choices=('all', 'none'), required=True)
    a = ap.parse_args()
    errs, linked = judge(Path(a.archive_nm).read_text(), Path(a.image_syms).read_text(), a.expect)
    for m in A.MEMBERS:
        print(f'{m}: {"LINKED " + ",".join(linked.get(m, [])[:2]) if linked.get(m) else "absent"}')
    for e in errs:
        print('FAIL ' + e)
    print(f'R4B RETENTION ({a.expect}) ' + ('PASS' if not errs else 'FAIL'))
    return 0 if not errs else 1


if __name__ == '__main__':
    sys.exit(main())
