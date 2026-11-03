#!/usr/bin/env python3
"""Judge of the I2 TSan line-A control pair and the shared TSan run classifier (codex 01:16; codex 01:23 CHANGES).

Only COMPLETE, fully parsed executions count.  stderr is parsed as the GCC ThreadSanitizer report format (the format
observed in i2ctl-g1/tsan-linea-drop-1, stated as the supported format; anything else is INVALID):
  ==================
  WARNING: ThreadSanitizer: data race (pid=N)
    <Read|Write|Atomic read|Atomic write> of size S at 0xA by <main thread|thread Tn>[ (mutexes: ...)]:   <- access 1
      #k frames
    Previous <read|write|...> of size S at 0xA by <...>[ (mutexes: ...)]:                               <- access 2
      #k frames
    Location is <global|heap block|stack ...> ...   [frames]
    Mutex Mn (0x...) created at:   frames                              (zero or more)
    Thread Tn (tid=N, <state>) created by <...> at:   frames          (zero or more)
  SUMMARY: ThreadSanitizer: data race <file:line> in <function of access 1 frame #0>
  ==================
  ... and, after the last report, exactly one 'ThreadSanitizer: reported N warnings' with N = the report count.
Frames are taken from the two ACCESS sections only (never from location / mutex / thread-creation sections).
Any other stderr line, a truncated block, a missing or extra access section, a mismatched summary or count is
INVALID.  stdout must hold the exact completion line 'I2 <case> PASS'; the line-A cases also need exactly one
well-formed LINEA receipt matching the case (drop/handoff values, positive drain count and read_idx, the initial
sample 0, a valid step-3 sample index whose sample equals read_idx).

  linea-drop          DETECTED     rc 66, >= 1 report, every report a data race between an access through
                                   line_a_store and a producer line-A access (p_getllar or its following p_putllc),
                                   at least one naming p_getllar
  linea-locked        CLEAN        rc 0, stderr empty
  linea-drop-handoff  NOT DETECTED rc 0, stderr empty (classify_drop maps the drop judge's clean run)
  observational       classify_observational: rc 0 + stderr empty -> NOT DETECTED; rc 66 + complete report(s) ->
                      DETECTED; anything else -> INVALID
Note: a DETECTED report may be on the line's bytes or on the emulation's version counter (g_ver), both accessed by
line_a_store and p_getllar; the judge proves the unlocked PAIR, the LINEA receipt separately proves the sampled
read_idx.

usage: tsan_pair_judge.py --selftest"""
import re
import sys

DELIM = '=================='
WARN = re.compile(r'^WARNING: ThreadSanitizer: (data race)(?: \(pid=\d+\))?$')
WHO = r'by (?:main thread|thread T\d+)(?: \(mutexes: [^)]*\))?:$'
ACC1 = re.compile(r'^  (?:Read|Write|Atomic read|Atomic write) of size \d+ at 0x[0-9a-f]+ ' + WHO)
ACC2 = re.compile(r'^  Previous (?:read|write|atomic read|atomic write) of size \d+ at 0x[0-9a-f]+ ' + WHO)
LOC = re.compile(r'^  Location is (?:global|heap block|stack of|thread T\d+|TLS) ')
MUTEX = re.compile(r'^  Mutex M\d+ \(0x[0-9a-f]+\) created at:$')
THREAD = re.compile(r'^  Thread T\d+ \(tid=\d+, [a-z]+\) created by (?:main thread|thread T\d+) at:$')
FRAME = re.compile(r'^    #\d+ (\S+) ')
SUMMARY = re.compile(r'^SUMMARY: ThreadSanitizer: data race .* in (\S+)$')
TOTAL = re.compile(r'^ThreadSanitizer: reported (\d+) warnings$')
RECEIPT = re.compile(r'^LINEA drop ([01]) handoff ([01]): read_idx after the drain (\d+) \((\d+) drains\); '
                     r'line-A getllar samples((?: \d+)+); step-3 samples from index (\d+)$')
CASES = {'linea-drop': (1, 0), 'linea-locked': (0, 0), 'linea-drop-handoff': (1, 1)}
PRODUCER = ('p_getllar', 'p_putllc')


class Bad(Exception):
    pass


def parse_tsan(stderr):
    """[(access1 frames, access2 frames)] of every complete data-race report; raises Bad on anything else"""
    lines = stderr.splitlines()
    i, reps, total = 0, [], None
    while i < len(lines):
        l = lines[i]
        if not l.strip():
            i += 1
            continue
        m = TOTAL.match(l)
        if m:
            if total is not None:
                raise Bad('two warning counts')
            total = int(m.group(1))
            i += 1
            continue
        if total is not None:
            raise Bad(f'output after the warning count: {l[:80]!r}')
        if l != DELIM:
            raise Bad(f'unexplained stderr line {l[:80]!r}')
        i += 1
        if i >= len(lines) or not WARN.match(lines[i]):
            raise Bad('a report block without its WARNING line')
        i += 1
        sections = []  # [kind, frames]
        summary = None
        while True:
            if i >= len(lines):
                raise Bad('a truncated report (no SUMMARY / closing delimiter)')
            l = lines[i]
            if l == DELIM:
                break
            if not l.strip():
                i += 1
                continue
            s = SUMMARY.match(l)
            if s:
                if summary is not None:
                    raise Bad('two SUMMARY lines in one report')
                summary = s.group(1)
                i += 1
                continue
            if summary is not None:
                raise Bad(f'a line after SUMMARY inside the report: {l[:80]!r}')
            f = FRAME.match(l)
            if f:
                if not sections:
                    raise Bad('a frame outside any section')
                sections[-1][1].append(f.group(1))
            elif ACC1.match(l):
                sections.append(['acc1', []])
            elif ACC2.match(l):
                sections.append(['acc2', []])
            elif LOC.match(l):
                sections.append(['loc', []])
            elif MUTEX.match(l):
                sections.append(['mutex', []])
            elif THREAD.match(l):
                sections.append(['thread', []])
            else:
                raise Bad(f'an unknown report line {l[:80]!r}')
            i += 1
        i += 1  # the closing delimiter
        kinds = [k for k, _ in sections]
        if kinds[:2] != ['acc1', 'acc2'] or kinds.count('acc1') != 1 or kinds.count('acc2') != 1:
            raise Bad(f'not exactly two access sections first ({kinds})')
        if any(k in ('acc1', 'acc2') for k in kinds[2:]):
            raise Bad('an extra access section')
        a1, a2 = sections[0][1], sections[1][1]
        if not a1 or not a2:
            raise Bad('an access section without frames')
        if summary is None or summary != a1[0]:
            raise Bad(f'SUMMARY {summary!r} does not match the first access frame {a1[0]!r}')
        reps.append((a1, a2))
    if reps and total != len(reps):
        raise Bad(f'warning count {total} != {len(reps)} parsed report(s)')
    if not reps and total is not None:
        raise Bad('a warning count with no report')
    return reps


def parse_receipt(case, stdout):
    lines = stdout.splitlines()
    if f'I2 {case} PASS' not in lines:
        raise Bad(f'no exact completion line "I2 {case} PASS"')
    if case not in CASES:
        return None
    rec = [l for l in lines if l.startswith('LINEA')]
    if len(rec) != 1:
        raise Bad(f'{len(rec)} LINEA receipt lines (want exactly 1)')
    m = RECEIPT.match(rec[0])
    if not m:
        raise Bad(f'a malformed LINEA receipt {rec[0][:80]!r}')
    drop, handoff, ridx, drains = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    samples = [int(x) for x in m.group(5).split()]
    k = int(m.group(6))
    if (drop, handoff) != CASES[case]:
        raise Bad(f'the receipt is for drop {drop} handoff {handoff}, not case {case}')
    if ridx <= 0 or drains <= 0:
        raise Bad(f'read_idx {ridx} / drains {drains} not positive')
    if samples[0] != 0:
        raise Bad(f'the first line-A sample {samples[0]} is not 0')
    if not 1 <= k < len(samples) or samples[k] != ridx:
        raise Bad(f'the step-3 sample (index {k} of {samples}) is not the stored read_idx {ridx}')
    return ridx


def _status(rc, timed_out):
    if timed_out:
        raise Bad('watchdog timeout')
    if rc is None or rc < 0:
        raise Bad(f'terminated by a signal or no status (rc {rc})')


def judge(case, rc, stdout, stderr, timed_out=False):
    try:
        _status(rc, timed_out)
        parse_receipt(case, stdout)
        reps = parse_tsan(stderr)
    except Bad as b:
        return 'INVALID', str(b)
    if not reps:
        if stderr.strip():
            return 'INVALID', 'stderr output that is not a report'
        return ('CLEAN' if rc == 0 else 'INVALID'), f'no report, rc {rc}'
    if rc != 66:
        return 'INVALID', f'report(s) with rc {rc} (want the configured TSan exit 66)'
    named = 0
    for a1, a2 in reps:
        sides = [set(a1), set(a2)]
        store = [j for j in (0, 1) if 'line_a_store' in sides[j]]
        if len(store) != 1 or not sides[1 - store[0]] & set(PRODUCER):
            return 'INVALID', f'a report outside the line-A pair ({a1[:3]} / {a2[:3]})'
        named += 'p_getllar' in sides[1 - store[0]]
    if not named:
        return 'INVALID', 'no report names the intended line_a_store / p_getllar pair'
    return 'DETECTED', f'{len(reps)} report(s), {named} with access stacks line_a_store / p_getllar'


def classify_drop(case, rc, stdout, stderr, timed_out=False):
    v, why = judge(case, rc, stdout, stderr, timed_out)
    return ('NOT DETECTED' if v == 'CLEAN' else v), why


def classify_observational(case, rc, stdout, stderr, timed_out=False):
    """the old pipeline control: race presence only is observational; the execution itself must be complete"""
    try:
        _status(rc, timed_out)
        parse_receipt(case, stdout)
        reps = parse_tsan(stderr)
    except Bad as b:
        return 'INVALID', str(b)
    if not reps:
        return ('NOT DETECTED', 'complete, rc 0, no sanitizer output') if rc == 0 and not stderr.strip() else \
            ('INVALID', f'rc {rc} with no report')
    if rc != 66:
        return 'INVALID', f'report(s) with rc {rc}'
    return 'DETECTED', f'{len(reps)} complete data-race report(s), rc 66'


# ---- self-test: real-format fixtures ----
_F = '    #{n} {fn} /src/x.c:{n} (i2-tsan+0x1{n}) (BuildId: ab)'


def _frames(*fns):
    return '\n'.join(_F.format(n=i, fn=f) for i, f in enumerate(fns))


def report(a1=('ps3dk_prof__psp_emul_version', 'p_getllar', 'snapshot'),
           a2=('ps3dk_prof__psp_emul_version_bump', 'line_a_store', 'm_st64'), extra='', summary=None):
    return (f'{DELIM}\nWARNING: ThreadSanitizer: data race (pid=725)\n'
            f'  Read of size 8 at 0x55555558af68 by thread T1:\n{_frames(*a1)}\n\n'
            f'  Previous write of size 8 at 0x55555558af68 by main thread (mutexes: write M0):\n{_frames(*a2)}\n\n'
            f"  Location is global 'g_ver' of size 2048 at 0x55555558a960 (i2-tsan+0x36f68)\n\n"
            f'  Mutex M0 (0x55555558a1d8) created at:\n{_frames("pthread_mutex_init", "line_a_store")}\n\n'
            f'  Thread T1 (tid=727, running) created by main thread at:\n{_frames("pthread_create", "p_getllar")}\n'
            f'{extra}\nSUMMARY: ThreadSanitizer: data race /src/psp_host_emul.c:32 in {summary or a1[0]}\n{DELIM}\n')


def selftest():
    rec = {'linea-drop': 'LINEA drop 1 handoff 0: read_idx after the drain 16 (1 drains); line-A getllar samples '
                         '0 16 16; step-3 samples from index 1',
           'linea-locked': 'LINEA drop 0 handoff 0: read_idx after the drain 16 (1 drains); line-A getllar samples '
                           '0 16 16; step-3 samples from index 1',
           'linea-drop-handoff': 'LINEA drop 1 handoff 1: read_idx after the drain 16 (1 drains); line-A getllar '
                                 'samples 0 16 16; step-3 samples from index 1'}

    def out(case, r=None):
        return f'{rec[case] if r is None else r}\nI2 {case} PASS\n'
    one = report() + 'ThreadSanitizer: reported 1 warnings\n'
    two = report() + report(a1=('memcpy', 'p_putllc'), a2=('memcpy', 'line_a_store')) + \
        'ThreadSanitizer: reported 2 warnings\n'
    unrelated = report(a1=('foo', 'bar'), a2=('baz', 'qux')) + 'ThreadSanitizer: reported 1 warnings\n'
    trunc = one[:one.index('  Location is global')]
    extra_acc = report(extra='  Read of size 4 at 0x1 by thread T2:\n    #0 zzz /x.c:1 (a+0x1)\n') + \
        'ThreadSanitizer: reported 1 warnings\n'
    D = 'linea-drop'
    rows = [
        ('the intended pair, complete, rc 66', judge(D, 66, out(D), one), 'DETECTED'),
        ('the pair also via p_putllc', judge(D, 66, out(D), two), 'DETECTED'),
        ('only a p_putllc report (the read is not named)',
         judge(D, 66, out(D), report(a1=('memcpy', 'p_putllc'), a2=('memcpy', 'line_a_store')) +
               'ThreadSanitizer: reported 1 warnings\n'), 'INVALID'),
        ('codex repro: truncated from the Location section on', judge(D, 66, out(D), trunc), 'INVALID'),
        ('codex repro: bogus receipt and completion', judge(D, 66, 'LINEA drop nonsense\nI2 linea-drop PASS_BOGUS\n',
                                                            one), 'INVALID'),
        ('a receipt of another case', judge(D, 66, out(D, rec['linea-locked']), one), 'INVALID'),
        ('a receipt whose step-3 sample is not the stored read_idx',
         judge(D, 66, out(D, rec[D].replace('samples 0 16 16', 'samples 0 8 16')), one), 'INVALID'),
        ('a receipt with read_idx 0', judge(D, 66, out(D, rec[D].replace('drain 16 ', 'drain 0 ')), one), 'INVALID'),
        ('an extra access section', judge(D, 66, out(D), extra_acc), 'INVALID'),
        ('pair names only in location/mutex/thread sections',
         judge(D, 66, out(D), report(a1=('memcpy', 'foo'), a2=('memcpy', 'bar')) +
               'ThreadSanitizer: reported 1 warnings\n'), 'INVALID'),
        ('a SUMMARY that does not match access 1', judge(D, 66, out(D), report(summary='other') +
                                                         'ThreadSanitizer: reported 1 warnings\n'), 'INVALID'),
        ('a warning count that does not match', judge(D, 66, out(D), report() +
                                                     'ThreadSanitizer: reported 2 warnings\n'), 'INVALID'),
        ('a missing warning count', judge(D, 66, out(D), report()), 'INVALID'),
        ('an unrelated race', judge(D, 66, out(D), unrelated), 'INVALID'),
        ('the pair plus an unrelated race', judge(D, 66, out(D), report() + report(a1=('foo', 'bar'),
                                                                            a2=('baz', 'qux')) +
                                                 'ThreadSanitizer: reported 2 warnings\n'), 'INVALID'),
        ('an unexplained stderr line', judge(D, 66, out(D), 'hello\n' + one), 'INVALID'),
        ('the expected report with rc 1', judge(D, 1, out(D), one), 'INVALID'),
        ('the expected report, killed by a signal', judge(D, -11, out(D), one), 'INVALID'),
        ('the expected report, missing completion', judge(D, 66, rec[D] + '\n', one), 'INVALID'),
        ('a TSan startup fatal', judge(D, 66, '', 'FATAL: ThreadSanitizer: unexpected memory mapping\n'), 'INVALID'),
        ('a watchdog timeout', judge(D, None, '', '', timed_out=True), 'INVALID'),
        ('clean locked run', judge('linea-locked', 0, out('linea-locked'), ''), 'CLEAN'),
        ('locked, rc 1, no report', judge('linea-locked', 1, out('linea-locked'), ''), 'INVALID'),
        ('locked, rc 66, no report', judge('linea-locked', 66, out('linea-locked'), ''), 'INVALID'),
        ('handoff mutant complete, no report -> NOT DETECTED',
         classify_drop('linea-drop-handoff', 0, out('linea-drop-handoff'), ''), 'NOT DETECTED'),
        ('handoff mutant crashed -> INVALID', classify_drop('linea-drop-handoff', -6, '', ''), 'INVALID'),
        ('observational: complete rc 0, no output', classify_observational('basic-thread-drop-ma', 0,
                                                                           'I2 basic-thread-drop-ma PASS\n', ''),
         'NOT DETECTED'),
        ('observational: complete rc 66 with a report', classify_observational(
            'basic-thread-drop-ma', 66, 'I2 basic-thread-drop-ma PASS\n', one), 'DETECTED'),
        ('observational: rc 1 with no report', classify_observational('basic-thread-drop-ma', 1,
                                                                      'I2 basic-thread-drop-ma PASS\n', ''),
         'INVALID'),
        ('observational: rc 66 with no report', classify_observational('basic-thread-drop-ma', 66,
                                                                       'I2 basic-thread-drop-ma PASS\n', ''),
         'INVALID'),
        ('observational: a report with rc 3', classify_observational('basic-thread-drop-ma', 3,
                                                                     'I2 basic-thread-drop-ma PASS\n', one),
         'INVALID'),
        ('observational: a truncated report', classify_observational('basic-thread-drop-ma', 66,
                                                                     'I2 basic-thread-drop-ma PASS\n', trunc),
         'INVALID'),
    ]
    bad = 0
    for name, (got, why), want in rows:
        ok = got == want
        print(f'{"PASS" if ok else "FAIL"} judge {name}: {got} ({why})')
        bad += not ok
    print('TSAN PAIR JUDGE SELFTEST ' + ('PASS' if not bad else f'FAIL ({bad})') + f' ({len(rows)} rows)')
    return 0 if not bad else 1


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        sys.exit(selftest())
    print(__doc__)
    sys.exit(2)
