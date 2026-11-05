#!/usr/bin/env python3
"""The ONE TSan decision for the E1 host rows (review 10:51): status, completion and report together.

The report parser is the reviewed I2 strict parser (tests/host/ps3dk-prof-spu-int/tsan_pair_judge.py parse_tsan):
every stderr line classified, complete === delimiters, exactly two access sections first, frames taken from those
two sections only, SUMMARY equal to the first access's frame #0, exactly one final warning count equal to the
report count; anything else is INVALID.

  judge(kind, rc, timed_out, reached, expected, stderr) -> (verdict, why)
    kind 'clean'    (a plain TSan row): CLEAN iff rc == the row's expected rc, the markers reached == expected and
                    stderr is EMPTY; otherwise INVALID
    kind 'negative' (the plain-commit-store control): REPORTED iff rc 66, the run completed (reached == expected: the
                    clean 28-marker inventory), >= 1 parsed report and EVERY report is a data race whose two ACCESS
                    stacks are the producer's commit store (h_report) on one side and the PPU's commit load
                    (e1_commit_ld or its caller e1_wait_report) on the other; otherwise INVALID (never REPORTED)
  A signal, a watchdog timeout or no status is INVALID for both kinds.

usage: e1_tsan_judge.py --selftest REAL_NEGATIVE_RUN_LOG   (the controls are built from a real report)
       e1_tsan_judge.py --classify KIND RUN_LOG RUNDIR EXPECTED_SEQS EXPECTED_RC   (a saved row)"""
from pathlib import Path
import sys

sys.dont_write_bytecode = True
_I2 = Path(__file__).resolve().parents[4] / 'tests' / 'host' / 'ps3dk-prof-spu-int'
sys.path.insert(0, str(_I2))
from tsan_pair_judge import Bad, parse_tsan  # noqa: E402  (the reviewed I2 strict parser)

STORE = {'h_report'}
LOAD = {'e1_commit_ld', 'e1_wait_report'}


def judge(kind, rc, timed_out, reached, expected, stderr, expected_rc=0):
    if timed_out:
        return 'INVALID', 'watchdog timeout'
    if rc is None or rc < 0:
        return 'INVALID', f'terminated by a signal or no status (rc {rc})'
    if sorted(reached) != sorted(expected):
        return 'INVALID', f'incomplete run: markers {sorted(reached)} != {sorted(expected)}'
    try:
        reps = parse_tsan(stderr)
    except Bad as b:
        return 'INVALID', f'stderr is not complete TSan reports: {b}'
    if kind == 'clean':
        if reps or stderr.strip():
            return 'INVALID', f'{len(reps)} report(s) / stderr output on a row that must be silent'
        if rc != expected_rc:
            return 'INVALID', f'rc {rc} != {expected_rc}'
        return 'CLEAN', f'rc {rc}, no report, {len(reached)} markers'
    if kind != 'negative':
        return 'INVALID', f'unknown kind {kind}'
    if rc != 66:
        return 'INVALID', f'rc {rc} (want the TSan exit 66)'
    if not reps:
        return 'INVALID', 'no report'
    for a1, a2 in reps:
        s1, s2 = set(a1), set(a2)
        if not ((s1 & STORE and s2 & LOAD) or (s2 & STORE and s1 & LOAD)):
            return 'INVALID', f'a report outside the commit store/load pair (access stacks {a1[:3]} / {a2[:3]})'
    return 'REPORTED', f'{len(reps)} complete report(s) on the h_report store / e1_commit_ld load access stacks, rc 66'


def stderr_of(run_log_text):
    """the stderr a test_e1 run.log recorded (format: cmd, rc, stdout, '--- stderr', stderr)"""
    head, sep, err = run_log_text.partition('\n--- stderr\n')
    if not sep:
        raise ValueError('not a run.log')
    rc = int(head.split('\n\nrc ', 1)[1].split('\n', 1)[0])
    return rc, err


def selftest(real_log):
    rc, err = stderr_of(Path(real_log).read_text())
    M = list(range(1, 29))
    rows = []

    def row(name, verdict_want, text, rc_=66, reached=M, kind='negative'):
        v, why = judge(kind, rc_, False, reached, M, text)
        ok = v == verdict_want
        rows.append(ok)
        print(f'{"ok  " if ok else "FAIL"} {name}: {v} ({why})')
    row('real report', 'REPORTED', err, rc)
    lines = err.split('\n')
    summ = [i for i, l in enumerate(lines) if l.startswith('SUMMARY: ')]
    row('missing SUMMARY', 'INVALID', '\n'.join(l for i, l in enumerate(lines) if i not in summ))
    unk = list(lines)
    unk.insert(summ[0] - 1, '  UNCLASSIFIED GARBAGE')
    row('unknown body line', 'INVALID', '\n'.join(unk))
    acc = [i for i, l in enumerate(lines) if l.startswith('  Previous write of size')]
    extra = list(lines)
    extra[acc[0]:acc[0]] = ['  Previous read of size 4 at 0x1 by main thread:', '    #0 h_report x (y)', '']
    row('extra access section', 'INVALID', '\n'.join(extra))
    # the expected names only in a NON-access section: the access frames renamed, h_report / e1_commit_ld moved into
    # the thread-creation section
    moved = [l.replace(' h_report ', ' other_store ').replace(' e1_commit_ld ', ' other_load ')
                .replace(' e1_wait_report ', ' other_wait ') for l in lines]
    moved = [l.replace('in e1_commit_ld', 'in other_load') for l in moved]
    tcr = [i for i, l in enumerate(moved) if l.startswith('  Thread T') and 'created by' in l][0]
    moved[tcr + 1:tcr + 1] = ['    #0 h_report a (b)', '    #1 e1_commit_ld a (b)']
    row('expected names only in a non-access section', 'INVALID', '\n'.join(moved))
    row('truncated report', 'INVALID', err[:err.rfind('SUMMARY')])
    row('wrong warning count', 'INVALID', err.replace('reported 1 warnings', 'reported 2 warnings'))
    row('missing warning count', 'INVALID', '\n'.join(l for l in lines if not l.startswith('ThreadSanitizer: reported')))
    row('report with rc 0', 'INVALID', err, 0)
    row('report with rc 1', 'INVALID', err, 1)
    row('incomplete run (27 markers)', 'INVALID', err, rc, M[:-1])
    row('a signal', 'INVALID', err, -11)
    row('clean row: empty stderr', 'CLEAN', '', 0, M, 'clean')
    row('clean row: a report', 'INVALID', err, 0, M, 'clean')
    row('clean row: stray stderr', 'INVALID', 'warning: x\n', 0, M, 'clean')
    row('clean row: wrong rc', 'INVALID', '', 1, M, 'clean')
    print(f'E1-TSAN-JUDGE {"PASS" if all(rows) else "FAIL"} {sum(rows)}/{len(rows)}')
    return 0 if all(rows) else 1


if __name__ == '__main__':
    if sys.argv[1:2] == ['--selftest'] and len(sys.argv) == 3:
        sys.exit(selftest(sys.argv[2]))
    if sys.argv[1:2] == ['--classify'] and len(sys.argv) == 7:
        # --classify KIND RUN_LOG RUNDIR EXPECTED(comma list of marker seqs) EXPECTED_RC: a saved row, reclassified
        rc, err = stderr_of(Path(sys.argv[3]).read_text())
        reached = sorted(int(p.name[-5:-3]) for p in Path(sys.argv[4]).glob('extp-*-*.mk'))
        want = [int(x) for x in sys.argv[5].split(',')]
        v, why = judge(sys.argv[2], rc, False, reached, want, err, int(sys.argv[6]))
        print(f'{v}: {why}')
        sys.exit(0 if v in ('CLEAN', 'REPORTED') else 1)
    sys.exit(__doc__)
