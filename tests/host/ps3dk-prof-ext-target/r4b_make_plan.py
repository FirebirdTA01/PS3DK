#!/usr/bin/env python3
"""P3.3 R4b shipped-build invariance (review 21:23 carry 2): for the shipped goals, the REACHABLE
dependency graph (each reachable target's prerequisites and recipe lines, from `make -p -n GOAL`) and the dry-run
recipe output (`make -n GOAL`) are identical between two sdk/libps3dkprof trees, and neither mentions the extension
(ext-ilp32 / ext-lp64 / ext-live / ilp32-ext / lp64-ext / libps3dkprof_ext / EXT_ / EXT64 / EXTL).  And for each
existing extension goal of the base tree (ext-ilp32, ext-ilp32-profile), the dry-run recipe output is identical too
(the LP64 twin and the live targets must not change a single ILP32 extension command).  Nothing is built or installed
(-n).

Since director decision D1, all and install also build and install the extension's release form, so the shipped
archives have their own goals, `shipped` and `install-shipped`.  With --pre-d1-base the BASE tree is one from before
D1 and its '', all and install goals are compared with the candidate's shipped, shipped and install-shipped (the goal
target itself renamed, nothing else); and the candidate's ext-lp64-profile dry run must equal the base's
ext-ilp32-profile with the LP64 command-line overrides it replaces.  Without the flag both trees use the new goal names.

Normalization, and nothing else: each tree's absolute root is replaced by the token <ROOT>.  The same scratch
PS3_BUILD_ROOT / PS3DK / PS3DEV values and SDK_VERSION=r4b-plan (a command-line override, so version.sh is not run)
are passed to both.  Any difference is printed in full.

usage: r4b_make_plan.py BASE_LIBDIR CAND_LIBDIR SCRATCH [--pre-d1-base]   (exit 0 iff identical and extension-free)"""
import re
import subprocess
import sys

NEW_GOALS = [('shipped', 'shipped'), ('install-shipped', 'install-shipped')]
PRE_D1_GOALS = [('', 'shipped'), ('all', 'shipped'), ('install', 'install-shipped')]
PRE_D1_RENAME = {'shipped': 'all', 'install-shipped': 'install'}
LP64_OVERRIDE = ['ext-ilp32-profile', 'EXTP_DIR=$(BUILD)/lp64-ext-profile',
                 'EXTP_FLAGS=$(EXT_FLAGS_lp64) -DPS3DK_PROF_WITH_PROFILE']
EXT = re.compile(r'ext-ilp32|ext-lp64|ext-live|ilp32-ext|lp64-ext|libps3dkprof_ext|EXT_|EXT64|EXTL')
EXT_GOALS = ['ext-ilp32', 'ext-ilp32-profile']  # dry-run identical between the trees


def make(libdir, scratch, args):
    cmd = ['make', '-C', libdir, '--no-print-directory', 'SDK_VERSION=r4b-plan', f'PS3DEV={scratch}/ps3dev',
           f'PS3DK={scratch}/ps3dk', f'PS3_BUILD_ROOT={scratch}/build', *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout.replace(libdir, '<ROOT>'), r.stderr.replace(libdir, '<ROOT>')


def graph(db, goal_targets):
    """rules in the '# Files' section: target -> (prereqs, recipe lines); then the subgraph reachable from goals"""
    rules, cur, in_files = {}, None, False
    for l in db.splitlines():
        if l.startswith('# Files'):
            in_files = True
            continue
        if l.startswith('# files hash-table stats') or l.startswith('# VPATH Search Paths'):
            in_files = False
        if not in_files:
            continue
        if l.startswith('\t'):
            if cur:
                rules[cur][1].append(l)
            continue
        m = re.match(r'^([^#\s][^=]*?)::?\s*(.*)$', l)
        if m and ':=' not in l and ' = ' not in l:
            cur = m.group(1)
            rules.setdefault(cur, ([], []))
            rules[cur][0].extend(m.group(2).replace('|', ' ').split())
        elif not l.strip():
            cur = None
    seen, stack = {}, list(goal_targets)
    while stack:
        t = stack.pop()
        if t in seen or t not in rules:
            continue
        seen[t] = rules[t]
        stack += rules[t][0]
    return seen


def renamed(g, rename):
    """the graph with each target and prerequisite name mapped through rename (identity when absent)"""
    return {rename.get(t, t): ([rename.get(p, p) for p in pre], rec) for t, (pre, rec) in g.items()}


def main():
    argv = sys.argv[1:]
    pre_d1 = '--pre-d1-base' in argv
    argv = [a for a in argv if a != '--pre-d1-base']
    if len(argv) != 3:
        print(__doc__)
        return 2
    base, cand, scratch = argv
    fail = 0
    for bg_goal, cg_goal in (PRE_D1_GOALS if pre_d1 else NEW_GOALS):
        name = f'{bg_goal or "(default)"}/{cg_goal}'
        gfail = fail
        fail = 0
        out = {}
        for tag, d, g in (('base', base, bg_goal), ('cand', cand, cg_goal)):
            args = [g] if g else []
            rc1, dry, e1 = make(d, scratch, ['-n', *args])
            rc2, db, e2 = make(d, scratch, ['-p', '-n', *args])
            if rc1 != 0 or rc2 != 0:
                print(f'FAIL {name} {tag}: make rc {rc1}/{rc2}: {(e1 + e2)[:400]}')
                fail = 1
                continue
            goals = [g] if g else ['all']
            gr = graph(db, goals)
            if tag == 'cand' and pre_d1:
                gr = renamed(gr, PRE_D1_RENAME)
            out[tag] = (dry, gr)
        if len(out) != 2:
            fail |= gfail
            continue
        (bd, bg), (cd, cg) = out['base'], out['cand']
        if bd != cd:
            print(f'FAIL {name}: dry-run output differs\n--- base\n{bd}\n--- cand\n{cd}')
            fail = 1
        if bg != cg:
            print(f'FAIL {name}: reachable graph differs')
            for t in sorted(set(bg) | set(cg)):
                if bg.get(t) != cg.get(t):
                    print(f'  {t}:\n    base {bg.get(t)}\n    cand {cg.get(t)}')
            fail = 1
        hits = [l for l in cd.splitlines() if EXT.search(l)] + [t for t in cg if EXT.search(t + ' '.join(cg[t][0]))]
        if hits:
            print(f'FAIL {name}: the extension appears in the shipped plan: {hits[:5]}')
            fail = 1
        if not fail:
            print(f'PASS {name}: dry-run identical ({len(cd.splitlines())} lines), reachable graph identical '
                  f'({len(cg)} targets), extension-free')
        fail |= gfail
    for g in EXT_GOALS:
        out = {}
        for tag, d in (('base', base), ('cand', cand)):
            rc, dry, err = make(d, scratch, ['-n', g])
            if rc != 0:
                print(f'FAIL {g} {tag}: make rc {rc}: {err[:400]}')
                fail = 1
            elif not dry.strip():
                print(f'FAIL {g} {tag}: empty dry run')
                fail = 1
            else:
                out[tag] = dry
        if len(out) != 2:
            continue
        if out['base'] != out['cand']:
            print(f'FAIL {g}: dry-run output differs')
            print('--- base')
            print(out['base'])
            print('--- cand')
            print(out['cand'])
            fail = 1
        else:
            print(f'PASS {g}: dry-run identical ({len(out["cand"].splitlines())} lines)')
    if pre_d1:
        rb, db_, eb = make(base, scratch, ['-n', *LP64_OVERRIDE])
        rc_, dc, ec = make(cand, scratch, ['-n', 'ext-lp64-profile'])
        if rb != 0 or rc_ != 0 or not dc.strip():
            print(f'FAIL ext-lp64-profile: make rc {rb}/{rc_}: {(eb + ec)[:400]}')
            fail = 1
        elif db_ != dc:
            print('FAIL ext-lp64-profile: dry run differs from the base LP64 override')
            print('--- base override')
            print(db_)
            print('--- cand ext-lp64-profile')
            print(dc)
            fail = 1
        else:
            print(f'PASS ext-lp64-profile: dry run equals the base LP64 override ({len(dc.splitlines())} lines)')
    print('R4B MAKE PLAN ' + ('PASS' if not fail else 'FAIL'))
    return fail


if __name__ == '__main__':
    sys.exit(main())
