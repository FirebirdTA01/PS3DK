#!/usr/bin/env python3
"""P3.3 R4b real-mutant judge (review 22:49).  A mutant is KILLED only when the image checker ran to completion and
reported, for its named row, the expected verdict with the named reason:
  - the checker's rc is exactly 1 (its normal NOT PASS exit; 0, 2 or a crash code is INVALID);
  - the report holds EXACTLY the expected row inventory (each (check, name) once, nothing else), every row a
    MACHINE-PROVED / FAIL / UNMET line, and the last non-blank line is exactly 'R4B IMAGE NOT PASS';
  - no traceback or other unclassified line anywhere;
  - the named row carries the expected verdict and the reason substring;
  - AND (review 23:01) the BASE image of the same run reports that same row MACHINE-PROVED in a complete base report;
    a base FAIL/UNMET there makes the mutation INCONCLUSIVE even if the mutant's reason matches.
Anything else is INVALID (never a kill); a complete report whose named row differs is NOT KILLED.

usage: r4b_mutjudge.py BASE_RC BASE_REPORT RC REPORT VERDICT CHECK NAME REASON
       prints KILLED / NOT KILLED / INCONCLUSIVE / INVALID; exit 0 iff KILLED
       r4b_mutjudge.py --selftest                              the judge's own controls"""
import re
import sys
from pathlib import Path

ROWS = [('G4b', 'm_st64'), ('G4b', 'm_st16'), ('G4c', 'm_ld64'), ('G4d', 'm_ld_line'), ('G4e', 'm_lwsync'),
        ('G4e', 'm_sync'), ('G4e', 'm_isync'), ('G4a', 'm_alloc'), ('G4f', 'ps3dk_prof_gpu_attach'),
        ('G4f', 'image symbols')]
LINE = re.compile(r'^(MACHINE-PROVED|FAIL|UNMET) (G4[a-f]) ([^:]+): (.*)$')
EXPECTED_MUTANTS = ('c1_st64_bytes', 'c2_lwsync_noop', 'c3_sync_before_memset', 'c4_bypass', 'c5_untaken_sync',
                    'c6_ldline_byte', 'c7_icall_in_window', 'c8_nonzero_fill', 'c9_wrong_dest', 'c10_ld64_lost',
                    'c11_st16_word')


def parse_report(rc, text, terminals):
    """(rows, None) for a complete report whose rc matches its terminal marker, else (None, reason)"""
    lines = [l for l in text.splitlines() if l.strip()]
    if not lines or lines[-1] not in terminals:
        return None, f'no terminal marker {terminals} (truncated or crashed report)'
    want_rc = {'R4B IMAGE PASS': 0, 'R4B IMAGE NOT PASS': 1}[lines[-1]]
    if rc != want_rc:
        return None, f'checker rc {rc} (want exactly {want_rc} with {lines[-1]!r})'
    rows = {}
    for l in lines[:-1]:
        m = LINE.match(l)
        if not m:
            return None, f'unclassified report line {l[:80]!r}'
        k = (m.group(2), m.group(3))
        if k in rows:
            return None, f'duplicate row {k}'
        rows[k] = (m.group(1), m.group(4))
    if sorted(rows) != sorted(ROWS):
        return None, f'row inventory {sorted(rows)} differs from the expected {len(ROWS)} rows'
    return rows, None


def judge(rc, text, verdict, check, name, reason, base_rc=None, base_text=None):
    rows, why = parse_report(rc, text, ('R4B IMAGE NOT PASS',))
    if rows is None:
        return 'INVALID', why
    if base_text is not None:
        base, bwhy = parse_report(base_rc, base_text, ('R4B IMAGE PASS', 'R4B IMAGE NOT PASS'))
        if base is None:
            return 'INVALID', f'base report: {bwhy}'
        if base[(check, name)][0] != 'MACHINE-PROVED':
            return 'INCONCLUSIVE', f'the base row {check} {name} is {base[(check, name)][0]}, not MACHINE-PROVED'
    want = {'PASS': 'MACHINE-PROVED'}.get(verdict, verdict)
    got_v, got_r = rows[(check, name)]
    if got_v == want and reason in got_r:
        return 'KILLED', f'{got_v} {check} {name}: {got_r[:160]}'
    return 'NOT KILLED', f'{got_v} {check} {name}: {got_r[:160]}'


def pin_mutants(tsv_text):
    names = [l.split('\t')[0] for l in tsv_text.splitlines() if l.strip()]
    if len(names) != len(set(names)) or sorted(names) != sorted(EXPECTED_MUTANTS):
        return f'MUTANTS.tsv names {names} differ from the expected {len(EXPECTED_MUTANTS)} unique controls'
    return None


def selftest():
    full = [f'MACHINE-PROVED {c} {n}: ok' for c, n in ROWS]
    good = full[:]
    good[0] = 'FAIL G4b m_st64: 8 stores to the EA on a path'
    rep = '\n'.join(good) + '\nR4B IMAGE NOT PASS\n'
    base_pass = '\n'.join(full) + '\nR4B IMAGE PASS\n'
    cases = [
        ('KILL with a passing base row', judge(1, rep, 'FAIL', 'G4b', 'm_st64', '8 stores', 0, base_pass), 'KILLED'),
        ('base fails the same row (the t3 c9 case)', judge(1, rep, 'FAIL', 'G4b', 'm_st64', '8 stores', 1, rep),
         'INCONCLUSIVE'),
        ('base report truncated', judge(1, rep, 'FAIL', 'G4b', 'm_st64', '8 stores', 0, '\n'.join(full[:3])),
         'INVALID'),
        ('base rc inconsistent with its marker', judge(1, rep, 'FAIL', 'G4b', 'm_st64', '8 stores', 1, base_pass),
         'INVALID'),
        ('a complete report with the named FAIL', judge(1, rep, 'FAIL', 'G4b', 'm_st64', '8 stores'), 'KILLED'),
        ('the named failure, then a traceback', judge(1, '\n'.join(good) + '\nTraceback (most recent call last):\n'
                                                     'R4B IMAGE NOT PASS\n', 'FAIL', 'G4b', 'm_st64', '8 stores'),
         'INVALID'),
        ('the named failure, then truncation', judge(1, '\n'.join(good[:4]) + '\n', 'FAIL', 'G4b', 'm_st64',
                                                     '8 stores'), 'INVALID'),
        ('a missing row', judge(1, '\n'.join(good[:-1]) + '\nR4B IMAGE NOT PASS\n', 'FAIL', 'G4b', 'm_st64',
                                '8 stores'), 'INVALID'),
        ('a duplicated row', judge(1, '\n'.join(good + good[:1]) + '\nR4B IMAGE NOT PASS\n', 'FAIL', 'G4b',
                                   'm_st64', '8 stores'), 'INVALID'),
        ('rc 0', judge(0, rep, 'FAIL', 'G4b', 'm_st64', '8 stores'), 'INVALID'),
        ('a crash rc', judge(2, rep, 'FAIL', 'G4b', 'm_st64', '8 stores'), 'INVALID'),
        ('a front-end-only report', judge(1, 'UNMET front x.elf: tool failure\nR4B IMAGE NOT PASS\n', 'FAIL',
                                          'G4b', 'm_st64', '8 stores'), 'INVALID'),
        ('the wrong reason', judge(1, rep, 'FAIL', 'G4b', 'm_st64', 'byte-reversed'), 'NOT KILLED'),
        ('the wrong verdict', judge(1, rep, 'UNMET', 'G4b', 'm_st64', '8 stores'), 'NOT KILLED'),
        ('the pinned mutant set', (pin_mutants('\n'.join(f'{n}\tx' for n in EXPECTED_MUTANTS)) or 'OK', ''), 'OK'),
        ('a mutant missing from the set', (pin_mutants('\n'.join(f'{n}\tx' for n in EXPECTED_MUTANTS[1:])) or 'OK',
                                           ''), 'MUTANTS.tsv'),
        ('a duplicated mutant', (pin_mutants('\n'.join(f'{n}\tx' for n in EXPECTED_MUTANTS + ('c1_st64_bytes',)))
                                 or 'OK', ''), 'MUTANTS.tsv'),
    ]
    bad = 0
    for name, (got, why), want in cases:
        ok = got.startswith(want)
        print(f'{"PASS" if ok else "FAIL"} judge {name}: {got[:60]} ({why[:80]})')
        bad += not ok
    print('R4B MUTJUDGE SELFTEST ' + ('PASS' if not bad else f'FAIL ({bad})'))
    return 0 if not bad else 1


def main():
    a = sys.argv[1:]
    if a == ['--selftest']:
        return selftest()
    if len(a) == 2 and a[0] == '--pin':
        e = pin_mutants(Path(a[1]).read_text())
        print(e or 'MUTANTS.tsv: the 11 expected unique controls')
        return 1 if e else 0
    if len(a) != 8:
        print(__doc__)
        return 2
    base_rc, base_report, rc, report, verdict, check, name, reason = a
    try:
        text = Path(report).read_text()
        base_text = Path(base_report).read_text()
    except OSError as e:
        print(f'INVALID: report unreadable ({e})')
        return 1
    v, why = judge(int(rc), text, verdict, check, name, reason, int(base_rc), base_text)
    print(f'{v}: {why}')
    return 0 if v == 'KILLED' else 1


if __name__ == '__main__':
    sys.exit(main())
