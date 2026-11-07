#!/usr/bin/env python3
"""Extension live archive contract (make ext-live-ilp32 / ext-live-lp64): the live link members compiled with the
extension recipe of one data model, archived beside that model's libps3dkprof_ext.a.  Fixed here, not taken from the
build: the member set is exactly LIVE_MEMBERS (each once); each member has exactly one
Tag_GNU_Power_CellOS_Data_Model attribute, the selected data model; and no externally visible symbol is DEFINED in
both the live archive and the extension archive of the same data model (an app links both; a symbol defined twice
would make the link order choose an implementation).  Every nm / readelf record is classified or the check fails.

usage: r4b_live.py --data-model ILP32|LP64 --ar AR --nm NM --readelf READELF --live-archive LIVE.a
                   --ext-archive EXT.a          (exit 0 iff every check passes)
The judging functions take tool output text, so r4b_selftest.py drives them with synthetic inputs."""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import r4b_archive as A  # noqa: E402

LIVE_MEMBERS = ('live_agent.o', 'live_os_lv2.o', 'live_api.o')


def judge_members(ar_t_text):
    got = [l.strip() for l in ar_t_text.splitlines() if l.strip()]
    errs = [f'duplicate member {n}' for n in sorted(set(got)) if got.count(n) > 1]
    errs += [f'missing member {n}' for n in LIVE_MEMBERS if n not in got]
    errs += [f'extra member {n}' for n in got if n not in LIVE_MEMBERS]
    return errs


def judge_attributes(text, model):
    """readelf -A of the live archive: each member exactly one data-model tag, the selected one"""
    if model not in A.CONTRACTS:
        raise A.Bad(f'unknown data model {model!r}')
    tags, cur = {}, None
    for l in text.splitlines():
        s = l.strip()
        m = re.match(r'^File: .*\(([^)]+)\)$', s)
        if m:
            cur = m.group(1)
            if cur in tags:
                raise A.Bad(f'duplicate attribute block for {cur}')
            tags[cur] = []
        elif (m := re.match(r'^Tag_GNU_Power_CellOS_Data_Model:\s+(\S+)$', s)) and cur:
            tags[cur].append(m.group(1))
        elif s and s not in ('Attribute Section: gnu', 'File Attributes') and not s.startswith('Tag_'):
            raise A.Bad(f'unrecognised readelf -A record {s!r}')
    errs = [f'{n}: data model {tags.get(n)} (want exactly [{model}])' for n in LIVE_MEMBERS if tags.get(n) != [model]]
    errs += [f'attributes for a non-member {n}' for n in tags if n not in LIVE_MEMBERS]
    return errs


def global_defs(nm_text, members):
    """nm (default format) of an archive -> (externally visible definitions, errors); one block per member"""
    defs, blocks, errs = set(), [], []
    for l in nm_text.splitlines():
        if not l.strip():
            continue
        mm = A.NM_MEMBER.match(l)
        if mm:
            blocks.append(mm.group(1))
            continue
        if not blocks:
            errs.append(f'nm record before any member block {l.strip()!r}')
            continue
        if A.NM_UND.match(l):
            continue
        d = A.NM_DEF.match(l)
        if d and d.group(1) in A.GLOBAL_DEF:
            defs.add(d.group(2))
        elif not (d and d.group(1) in A.LOCAL_DEF):
            errs.append(f'unclassified nm record {l.strip()!r}')
    if sorted(blocks) != sorted(members) or len(blocks) != len(members):
        errs.append(f'nm member blocks {blocks} (want exactly one block for each of {list(members)})')
    return defs, errs


def judge_disjoint(live_nm, ext_nm):
    ld, e1 = global_defs(live_nm, LIVE_MEMBERS)
    ed, e2 = global_defs(ext_nm, A.MEMBERS)
    both = sorted(ld & ed)
    errs = [f'live archive: {e}' for e in e1] + [f'ext archive: {e}' for e in e2]
    errs += [f'defined in both archives: {n}' for n in both]
    if not ld:
        errs.append('live archive: no externally visible definition')
    return errs, len(ld), len(ed)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise A.Bad(f'tool failure {cmd[0]} rc {r.returncode}: {r.stderr[:300]}')
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-model', required=True, choices=sorted(A.CONTRACTS))
    for a in ('--ar', '--nm', '--readelf', '--live-archive', '--ext-archive'):
        ap.add_argument(a, required=True)
    a = ap.parse_args()
    errs = []
    try:
        errs += judge_members(run([a.ar, 't', a.live_archive]))
        errs += judge_attributes(run([a.readelf, '-A', a.live_archive]), a.data_model)
        e, nl, ne = judge_disjoint(run([a.nm, a.live_archive]), run([a.nm, a.ext_archive]))
        errs += e
        print(f'externally visible definitions: live {nl}, ext {ne}')
    except A.Bad as b:
        errs.append(str(b))
    for x in errs:
        print('FAIL ' + x)
    print(f'R4B LIVE ({a.data_model}) ' + ('PASS' if not errs else 'FAIL'))
    return 0 if not errs else 1


if __name__ == '__main__':
    sys.exit(main())
