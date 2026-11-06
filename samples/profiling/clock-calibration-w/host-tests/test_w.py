#!/usr/bin/env python3
"""Host tests of the W / W0 probe (source/main.c + w_marker.h under W_HOST_TEST, host-tests/w_host.h fakes) and of
validate_w_markers.py.  Host behaviour only: the fakes model LV2/SPU/RSX/filesystem results; nothing here describes
the console (the "guaranteed refusal" mailbox code is a MODEL property, not an LV2 contract).

Rows (exit 0 only if all pass):
  M. the matrix: every ALLOWED row (generated here, producer-possible) VALID; every FORBIDDEN row INVALID with its
     named reason (incl. bounds, widths, predecessors, unres/result consistency); the maximal record fits W_REC_MAX;
  X. cross-record runs the validator must flag INCONSISTENT for the named reason (a predecessor skipping a present
     marker; present records contradicting an earlier stop / quit / CSV failure) with derived facts WITHHELD;
  F. the producer's control flow under injected faults (W and W0, plain and non-recovering ASan/UBSan, stderr
     empty), judged against a PINNED spec per case: the exact marker-file inventory and validity (failed-marker files
     included), the (seq, result) sequence, the attempted-marker order with one open and one close per opened marker,
     independent counters (SPU requests, RSX pairs, CSV fflush/fsync/fclose/raw close/open), the process rc, the
     validator's verdict and facts, the CSV inventory, and the analyzer's rc AND verdict (stderr empty);
  J. judge controls (each must FAIL for its named reason): an earlier marker deleted on a logging-failure run, the
     required CSV deleted, an extra request injected after a stopped domain, failed and empty analyzer output;
  T. exact traces for the pinned cases;  B. the build tag bound (CMake -P, _Static_assert, validator).
usage: test_w.py OUT"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
here = Path(__file__).resolve().parent
top = here.parent
sys.path.insert(0, str(here))
import validate_w_markers as V  # noqa: E402

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
BUILD, T, R = 'fedcba9876543210', 3, 2
TGT = f'{T},{R}'
ANALYZE = top.parent / 'clock-calibration/host-tests/analyze.py'
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
rows = []


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def validate(d, targets=TGT, build=BUILD):
    r = subprocess.run([sys.executable, '-I', str(here / 'validate_w_markers.py'), str(d), '--build', build,
                        '--targets', targets], capture_output=True, text=True)
    return r.returncode, r.stdout


# ================================================================ M. the matrix
PREV = {'W': {2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 7, 9: 8, 10: 9, 11: 10, 12: 11, 13: 12, 14: 13, 15: 14, 16: 15,
              17: 16},
        'W0': {2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 7, 9: 8, 10: 9, 12: 10, 13: 12, 14: 13, 15: 14, 16: 15, 17: 16}}
_runs = iter(range(1, 1 << 30))


def rec(seq, result, fl='', var='W', prev=None, unres=None, counts=None, run=None, tb=None):
    run = run or f'{next(_runs):016x}'
    tb = tb if tb is not None else 0x1000 + seq * 16
    if prev is None:
        prev = 'none' if seq == 1 else ('08:error:open:00000005' if seq == 99 else f'{PREV[var][seq]:02d}:ok') + \
            f':{tb - 8:016x}'
    if unres is None:
        s = 'u' if seq in (8, 12, 14) and result in ('timeout', 'identity') else \
            'q' if seq in (8, 12, 14) and result.startswith('mbox-error') else 'n'
        unres = (s, 'u' if seq == 10 and result in ('timeout', 'identity') else 'n')
    if counts is None:
        m = re.search(r'issued=(\d+)', fl)
        counts = (max(0, int(m.group(1)) - (1 if result in ('timeout', 'identity') else 0)) if seq == 8 and m else 0, 0)
    p = (f'CLKW1 run={run} variant={var} build={BUILD} seq={seq:02d} stage={V.STAGES[seq]} result={result} tb={tb:016x} '
         f'prev={prev} unres={unres[0]},{unres[1]} counts={counts[0]},{counts[1]}' + (f' {fl}' if fl else '')).encode()
    return run, seq, p + b' len=' + str(len(p)).encode() + b'\n'


def PH(n, csv='ok', seq=8, var='W'):
    last = n if seq in (8, 10) else T + n + (1 if var == 'W' else 0)
    return f'issued={n} last={last:08x} csv={csv}' if n else f'issued=0 csv={csv}'


H = '80010002'
NA = 'not-attempted'
ALLOWED = [(1, 'ok', '', 'W'), (2, 'ok', '', 'W'), (2, f'error:open:{H}', '', 'W'), (2, 'error:format:path', '', 'W'),
           (2, f'error:fdopen:{H}', 'rawclose=ok', 'W'), (2, f'error:fdopen:{H}', 'rawclose=00000009', 'W'),
           *[(s, 'ok', '', 'W') for s in (3, 5, 7, 9, 11, 13, 15)], (4, 'ok', '', 'W'), (4, 'skipped:csv-failed', '', 'W'),
           *[(4, f'error:{c}:{H}', '', 'W') for c in ('sysutil-register', 'init', 'import', 'group-create', 'thread-init',
                                                      'group-start')],
           (6, 'ok', '', 'W'), (6, f'unavailable:host-buffer:{H}', '', 'W'), (6, f'unavailable:gcm-init:{H}', '', 'W'),
           (6, 'skipped:csv-failed', '', 'W'), (6, 'skipped:spu-setup-failed', '', 'W'),
           (12, 'ok', 'dec=7fffec77 dec2=7fffec70 wrote=ffffffff csv=ok', 'W'), (12, 'timeout', 'csv=ok', 'W'),
           (12, 'identity', 'csv=fsync:00000005', 'W'), (12, f'mbox-refused:{H}', 'csv=ok', 'W'),
           (12, f'mbox-error:{H}', 'csv=ok', 'W'),
           *[(12, f'skipped:{r}', f'csv={NA}', 'W') for r in ('csv-failed', 'spu-setup-failed', 'spu-stopped', 'quit')],
           *[(12, f'skipped:{r}', f'csv={NA}', 'W0') for r in ('csv-failed', 'spu-setup-failed', 'variant-w0')],
           (16, 'ok', 'left_running=0 spu_join=ok', 'W'), (16, 'ok', 'left_running=0 spu_join=not-started', 'W'),
           (16, f'error:terminate:{H}', 'left_running=1 spu_join=not-attempted', 'W'),
           (16, f'error:join:{H}', 'left_running=1 spu_join=failed', 'W'),
           (16, f'error:destroy:{H}', 'left_running=0 spu_join=ok', 'W'),
           (16, f'error:image-close:{H}', 'left_running=0 spu_join=ok', 'W'),
           (16, 'skipped:csv-failed', 'left_running=0 spu_join=not-started', 'W'),
           (17, 'ok', 'fflush=ok fsync=ok fclose=ok', 'W'), (17, 'error', 'fflush=ok fsync=ok fclose=ok', 'W'),
           (17, 'error', f'fflush=00000005 fsync={NA} fclose=ok', 'W'), (17, 'error', 'fflush=ok fsync=00000005 fclose=ok', 'W'),
           (17, 'error', 'fflush=ok fsync=ok fclose=00000005', 'W'), (17, 'error', f'fflush={NA} fsync={NA} fclose=ok', 'W'),
           (17, 'skipped:csv-failed', f'fflush={NA} fsync={NA} fclose={NA}', 'W'), (99, 'failed', '', 'W')]
for s, t in ((8, T), (14, T), (10, R)):
    for var in (('W', 'W0') if s == 14 else ('W',)):
        A = lambda n, csv='ok': PH(n, csv, s, var)  # noqa: E731
        ALLOWED += [(s, 'ok', A(t), var), (s, 'ok', A(t, 'row-write'), var), (s, 'ok', A(t, 'fflush:00000005'), var),
                    (s, 'timeout', A(1), var), (s, 'identity', A(t), var), (s, 'quit', A(1), var),
                    (s, 'csv-stopped', A(1, NA), var), (s, 'skipped:csv-failed', A(0, NA), var),
                    (s, 'skipped:spu-setup-failed', A(0, NA), var)]
        if s != 10:
            ALLOWED += [(s, f'mbox-refused:{H}', A(0), var), (s, f'mbox-error:{H}', A(2), var)]
        else:
            ALLOWED += [(s, 'unavailable:label-address', A(0), var), (s, 'skipped:gcm-unavailable', A(0, NA), var),
                        (s, 'skipped:quit', A(0, NA), var)]
        if s == 14:
            ALLOWED += [(14, 'skipped:spu-stopped', A(0, NA), var), (14, 'skipped:quit', A(0, NA), var)]
FORBIDDEN = [  # (seq, result, fields, var, kw, named reason)
    (1, 'timeout', '', 'W', {}, 'cannot carry result'), (3, f'error:init:{H}', '', 'W', {}, 'cannot carry result'),
    (2, f'error:fopen:{H}', '', 'W', {}, 'cannot carry result'), (4, 'skipped:quit', '', 'W', {}, 'cannot carry result'),
    (8, 'skipped:quit', PH(0, NA), 'W', {}, 'cannot carry result'),
    (8, 'skipped:spu-stopped', PH(0, NA), 'W', {}, 'cannot carry result'),
    (10, f'mbox-error:{H}', PH(1, seq=10), 'W', {}, 'cannot carry result'),
    (12, 'skipped:variant-w0', f'csv={NA}', 'W', {}, 'cannot carry result'),
    (12, 'ok', 'dec=00000001 dec2=00000000 wrote=ffffffff csv=ok', 'W0', {}, 'cannot carry result'),
    (12, 'skipped:quit', f'csv={NA}', 'W0', {}, 'cannot carry result'),
    (11, 'ok', '', 'W0', {'prev': '10:ok:0000000000001008'}, 'not possible for variant'),
    (16, 'skipped:quit', 'left_running=0 spu_join=not-started', 'W', {}, 'cannot carry result'),
    (17, 'skipped:spu-setup-failed', f'fflush={NA} fsync={NA} fclose={NA}', 'W', {}, 'cannot carry result'),
    (99, 'ok', '', 'W', {}, 'cannot carry result'),
    (8, 'skipped:csv-failed', f'issued=1 last=00000001 csv={NA}', 'W', {}, 'issued=0'),
    (8, 'ok', PH(2), 'W', {}, 'ok needs issued'), (8, 'ok', 'issued=0 last=00000000 csv=ok', 'W', {}, 'last present iff'),
    (8, 'timeout', 'issued=2 csv=ok', 'W', {}, 'last present iff'), (8, 'timeout', PH(0), 'W', {}, 'follows a submitted'),
    (8, 'csv-stopped', PH(1), 'W', {}, 'checkpoint is not attempted'), (8, 'quit', PH(3), 'W', {}, 'issued < the target'),
    (10, 'unavailable:label-address', PH(1, seq=10), 'W', {}, 'nothing issued'),
    (12, 'timeout', 'dec=00000001 dec2=00000000 wrote=ffffffff csv=ok', 'W', {}, 'present iff ok'),
    (12, 'ok', 'csv=ok', 'W', {}, 'present iff ok'),
    (12, 'ok', 'dec=00000001 dec2=00000000 wrote=00000000 csv=ok', 'W', {}, 'written value'),
    (12, 'skipped:quit', 'csv=ok', 'W', {}, f'csv={NA}'),
    (16, f'error:join:{H}', 'left_running=0 spu_join=failed', 'W', {}, 'join failed'),
    (16, f'error:terminate:{H}', 'left_running=1 spu_join=ok', 'W', {}, 'terminate failed'),
    (16, 'ok', 'left_running=1 spu_join=ok', 'W', {}, 'completed terminate/join'),
    (17, 'error', 'fflush=00000005 fsync=ok fclose=ok', 'W', {}, 'fsync only after'),
    (17, 'ok', 'fflush=ok fsync=ok fclose=00000005', 'W', {}, 'ok needs'),
    (17, 'error', f'fflush=ok fsync=ok fclose={NA}', 'W', {}, 'fclose is attempted once'),
    (17, 'skipped:csv-failed', f'fflush={NA} fsync={NA} fclose=ok', 'W', {}, 'attempts nothing'),
    (5, 'ok', '', 'W', {'prev': '04:error:open:00000005:0000000000001048'}, 'successful predecessor'),
    (99, 'failed', '', 'W', {'prev': '08:ok:0000000000001630'}, 'failed predecessor'),
    (1, 'ok', '', 'W', {'prev': '00:ok:0000000000001008'}, 'prev=none'),
    (5, 'ok', '', 'W', {'prev': '03:ok:0000000000001048'}, 'cannot follow 03'),
    (4, 'ok', '', 'W', {'unres': ('u', 'n')}, 'unres before'),
    (2, 'error:open:5', '', 'W', {}, 'cannot carry result'), (2, 'error:open:8001000A', '', 'W', {}, 'cannot carry result'),
    (6, 'ok', 'issued=0 csv=ok', 'W', {}, 'fields'), (8, 'ok', '', 'W', {}, 'fields'),
    # review 20:15 (a)-(d) and the related consistency rules
    (8, 'timeout', PH(1), 'W', {'unres': ('n', 'n')}, 'needs SPU unres u'),
    (16, 'ok', 'left_running=0 spu_join=ok', 'W', {'prev': '00:ok:0000000000001100'}, 'cannot follow 00'),
    (3, 'ok', '', 'W', {'counts': (4294967296, 0)}, 'beyond the u32 width'),
    (14, 'ok', PH(3, seq=14), 'W', {'counts': (7, 0)}, 'build bounds'),
    (8, f'mbox-refused:{H}', PH(0), 'W', {'unres': ('q', 'n')}, 'invents no SPU unres'),
    (8, f'mbox-error:{H}', PH(0), 'W', {'unres': ('n', 'n')}, 'needs SPU unres q'),
    (10, 'timeout', PH(1, seq=10), 'W', {'unres': ('n', 'n')}, 'needs RSX unres u'),
    (14, 'ok', PH(3, seq=14), 'W', {'unres': ('u', 'n')}, 'unres must be n'),
    (14, 'ok', 'issued=3 last=00000003 csv=ok', 'W', {}, 'last 3 != 7'),
    (8, 'ok', PH(3), 'W', {'counts': (2, 0)}, 'SPU count'),
    (2, f'error:fdopen:{H}', '', 'W', {}, 'records the raw close'),
    (2, f'error:open:{H}', 'rawclose=ok', 'W', {}, 'rawclose only after'),
    (99, 'failed', '', 'W0', {'prev': '11:error:open:00000005:0000000000001100'}, 'not an attempted milestone'),
]
md = out / 'matrix'
md.mkdir()
want = {}
for seq, result, fl, var in ALLOWED:
    run, s, b = rec(seq, result, fl, var=var)
    (md / f'clockcal-{run}-{s:02d}.mk').write_bytes(b)
    want[f'clockcal-{run}-{s:02d}.mk'] = None
for seq, result, fl, var, kw, why in FORBIDDEN:
    run, s, b = rec(seq, result, fl, var=var, **kw)
    (md / f'clockcal-{run}-{s:02d}.mk').write_bytes(b)
    want[f'clockcal-{run}-{s:02d}.mk'] = why


def verdicts(vo):
    got = {}
    for l in vo.splitlines():
        m = re.match(r'(VALID|INVALID) (clockcal-\S+\.mk)(?:: (.*))?', l)
        if m:
            got[m.group(2)] = None if m.group(1) == 'VALID' else (m.group(3) or '')
    return got


rc, vo = validate(md)
got = verdicts(vo)
bad = [f'{n}: want {w!r} got {got.get(n, "MISSING")!r}' for n, w in want.items()
       if (w is None) != (got.get(n, 'x') is None) or (w is not None and w not in (got.get(n) or ''))]
row(f'matrix: {len(ALLOWED)} allowed VALID, {len(FORBIDDEN)} forbidden INVALID with their reasons',
    not bad and len(got) == len(want), '; '.join(bad[:5]))
# (b) the build bound: issued beyond the target of THIS build
bd = out / 'matrix-bound'
bd.mkdir()
run, s, b = rec(8, 'timeout', 'issued=1501 last=000005dd csv=ok', counts=(1500, 0))
(bd / f'clockcal-{run}-08.mk').write_bytes(b)
rc, vo = validate(bd, targets='1500,1500')
row('bound: 08 timeout issued=1501 with --targets 1500,1500 -> INVALID', rc == 1 and 'beyond the target 1500' in vo,
    vo.strip().splitlines()[0] if vo.strip() else '')
mx = rec(12, f'mbox-error:{H}', 'dec=ffffffff dec2=ffffffff wrote=ffffffff csv=fflush:ffffffff',
         counts=(4294967295, 4294967295), prev='11:ok:ffffffffffffffff', unres=('q', 'u'))[2]
bound = int(re.search(r'#define W_REC_MAX (\d+)', (top / 'source/w_marker.h').read_text()).group(1))
row('maximal record within W_REC_MAX', len(mx) < bound, f'{len(mx)} < {bound}')

# ================================================================ X. cross-record contradictions in PRESENT records
def xrun(name, recs, reason, withheld=True):
    d = out / f'x-{name}'
    d.mkdir()
    run = f'{0xabc000 + len(rows):016x}'
    for seq, result, fl, kw in recs:
        _r, s, b = rec(seq, result, fl, run=run, **kw)
        (d / f'clockcal-{run}-{s:02d}.mk').write_bytes(b)
    rc, vo = validate(d)
    inc = [l for l in vo.splitlines() if l.startswith('INCONSISTENT')]
    ok = rc == 1 and inc and all(reason in l for l in inc) and \
        ('INVALID' not in vo) and (not withheld or ('WITHHELD' in vo and 'SPU_PRODUCER_QUIESCED' not in vo
                                                    and 'WRITE_REQUEST_REPLY' not in vo))
    row(f'cross-record {name}', ok, next((l for l in vo.splitlines() if l.startswith('INCONSISTENT')), vo[:120]))


xrun('predecessor-skips-present-marker', [(6, 'ok', '', {}), (7, 'ok', '', {}),
                                          (8, 'ok', PH(3), {'prev': '06:ok:0000000000001078'})], 'lie between')
xrun('stop-then-later-begin', [(8, 'timeout', PH(2), {}), (13, 'ok', '', {'unres': ('u', 'n'), 'counts': (1, 0)}),
                               (16, 'ok', 'left_running=0 spu_join=ok', {'unres': ('u', 'n'), 'counts': (1, 0)})],
     'earlier present record stopped')
xrun('stop-then-write-ok', [(8, 'timeout', PH(2), {}),
                            (12, 'skipped:quit', f'csv={NA}', {'unres': ('u', 'n'), 'counts': (1, 0)})],
     'require skipped:spu-stopped')
xrun('quit-then-begin', [(8, 'quit', PH(1), {}), (9, 'ok', '', {'counts': (1, 0)})], 'earlier present record stopped')
xrun('csv-failure-then-ok', [(8, 'ok', PH(3, 'fflush:00000005'), {}),
                             (10, 'ok', PH(2, seq=10), {'prev': '08:ok:0000000000001098', 'counts': (3, 2)}),
                             (12, 'ok', 'dec=00000001 dec2=00000000 wrote=ffffffff csv=ok',
                              {'prev': '10:ok:00000000000010b8', 'counts': (3, 2)})],
     'require skipped:csv-failed')
xrun('setup-failure-then-gcm', [(4, f'error:init:{H}', '', {}), (5, 'ok', '', {})], 'earlier present record stopped')

# ================================================================ F. the producer's control flow
def build(tag, d, var, extra=()):
    exe = out / f'w-{tag}'
    r = subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DW_HOST_TEST', *extra,
                        *(['-DW_VARIANT_W0'] if var == 'W0' else []), f'-DCAL_SPU_SAMPLES={T}',
                        f'-DCAL_RSX_SAMPLES={R}', '-DCAL_PERIOD_US=0', f'-DW_DIR="{d}"', f'-DW_BUILD_ID="{BUILD}"',
                        f'-I{here}', f'-I{top.parent / "clock-calibration/source"}', str(top / 'source/main.c'),
                        '-o', str(exe)], capture_output=True, text=True)
    (out / f'w-{tag}.build.txt').write_text(r.stdout + r.stderr)
    return exe if r.returncode == 0 else None


FULL = {'W': [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17],
        'W0': [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17]}
COMPLETE = (0, 'VERDICT RECORD COMPLETE:')
NOTCLEAN = (3, 'VERDICT RECORD COMPLETE, RUN NOT CLEAN')


def INVALIDV(why):
    return (1, f'VERDICT INVALID: {why}')


class Spec:
    def __init__(self, name, var, faults, markers, rc, cnt, analyzer, facts=(), fields=None, invalid=(), csv=1):
        self.name, self.var, self.faults, self.markers, self.rc = name, var, faults, markers, rc
        self.cnt, self.analyzer, self.facts, self.fields, self.invalid, self.csv = \
            cnt, analyzer, list(facts), fields or {}, set(invalid), csv


def ok_markers(var, over=(), drop=()):
    m = {s: 'ok' for s in FULL[var]}
    for s in drop:
        m.pop(s, None)
    m.update(dict(over))
    return m


def skips(reason, frm):
    return {s: f'skipped:{reason}' for s in (6, 8, 10, 12, 14) if s >= frm}


SP = [  # counters cnt = (SPU mailbox writes, RSX pairs, CSV fflush, CSV fsync)
    Spec('normal', 'W', '', ok_markers('W'), 0, (7, 2, 5, 5), COMPLETE, ['SPU_PRODUCER_QUIESCED', 'WRITE_REQUEST_REPLY'],
         {8: ['issued=3 last=00000003 csv=ok'], 12: ['wrote=ffffffff'], 14: ['issued=3 last=00000007']}),
    Spec('normal-w0', 'W0', '', ok_markers('W0', {12: 'skipped:variant-w0'}), 0, (6, 2, 4, 4), COMPLETE,
         ['SPU_PRODUCER_QUIESCED'], {14: ['issued=3 last=00000006']}),
    Spec('p0-timeout', 'W', 'mb@2=timeout', ok_markers('W', {8: 'timeout', 12: 'skipped:spu-stopped',
                                                             14: 'skipped:spu-stopped'}, (11, 13)), 0, (2, 2, 3, 3),
         NOTCLEAN, ['UNRESOLVED_SUBMISSION spu', 'SPU_PRODUCER_QUIESCED'], {8: ['issued=2 last=00000002', 'unres=u,n']}),
    Spec('p0-identity', 'W', 'mb@1=identity', ok_markers('W', {8: 'identity', 12: 'skipped:spu-stopped',
                                                               14: 'skipped:spu-stopped'}, (11, 13)), 0, (1, 2, 3, 3),
         NOTCLEAN, ['UNRESOLVED_SUBMISSION spu'], {8: ['issued=1']}),
    Spec('p0-mbox-error', 'W', f'mb@1=err:{H}', ok_markers('W', {8: f'mbox-error:{H}', 12: 'skipped:spu-stopped',
                                                                 14: 'skipped:spu-stopped'}, (11, 13)), 0, (1, 2, 3, 3),
         NOTCLEAN, ['SUBMISSION_OUTCOME_UNKNOWN spu'], {8: ['issued=0 csv=ok', 'unres=q,n']}),
    Spec('p0-mbox-refused-model', 'W', 'mb@2=err:80010099',
         ok_markers('W', {8: 'mbox-refused:80010099', 12: 'skipped:spu-stopped', 14: 'skipped:spu-stopped'}, (11, 13)),
         0, (2, 2, 3, 3), NOTCLEAN, ['SUBMISSION_REFUSED spu (producer-classified)'],
         {8: ['issued=1 last=00000001', 'unres=n,n']}),
    Spec('rsx-timeout', 'W', 'rsx@1=timeout', ok_markers('W', {10: 'timeout'}), 0, (7, 1, 5, 5), NOTCLEAN,
         ['UNRESOLVED_SUBMISSION rsx'], {10: ['issued=1 last=00000001', 'unres=n,u'], 17: ['unres=n,u']}),
    Spec('rsx-identity', 'W', 'rsx@2=identity', ok_markers('W', {10: 'identity'}), 0, (7, 2, 5, 5), NOTCLEAN,
         ['UNRESOLVED_SUBMISSION rsx'], {10: ['issued=2 last=00000002']}),
    Spec('label-null', 'W', 'label=null', ok_markers('W', {10: 'unavailable:label-address'}), 0, (7, 0, 5, 5), NOTCLEAN,
         [], {10: ['issued=0 csv=ok']}),
    Spec('gcm-host-buffer', 'W', f'gcm.host-buffer={H}', ok_markers('W', {6: f'unavailable:host-buffer:{H}',
                                                                          10: 'skipped:gcm-unavailable'}, (9,)), 0,
         (7, 0, 4, 4), NOTCLEAN),
    Spec('gcm-init', 'W', f'gcm.init={H}', ok_markers('W', {6: f'unavailable:gcm-init:{H}',
                                                            10: 'skipped:gcm-unavailable'}, (9,)), 0, (7, 0, 4, 4), NOTCLEAN),
    Spec('write-timeout', 'W', 'mb@4=timeout', ok_markers('W', {12: 'timeout', 14: 'skipped:spu-stopped'}, (13,)), 0,
         (4, 2, 4, 4), NOTCLEAN, ['UNRESOLVED_SUBMISSION spu'], {12: ['csv=ok']}),
    Spec('write-mbox-error', 'W', f'mb@4=err:{H}', ok_markers('W', {12: f'mbox-error:{H}', 14: 'skipped:spu-stopped'},
                                                              (13,)), 0, (4, 2, 4, 4), NOTCLEAN,
         ['SUBMISSION_OUTCOME_UNKNOWN spu'], {12: ['unres=q,n']}),
    Spec('p1-timeout', 'W', 'mb@6=timeout', ok_markers('W', {14: 'timeout'}), 0, (6, 2, 5, 5), NOTCLEAN,
         ['UNRESOLVED_SUBMISSION spu'], {14: ['issued=2 last=00000006']}),
    Spec('w0-p1-timeout', 'W0', 'mb@5=timeout', ok_markers('W0', {12: 'skipped:variant-w0', 14: 'timeout'}), 0,
         (5, 2, 4, 4), NOTCLEAN, ['UNRESOLVED_SUBMISSION spu'], {14: ['issued=2 last=00000005']}),
    Spec('quit-p0', 'W', 'quit@2', ok_markers('W', {8: 'quit', **{s: 'skipped:quit' for s in (10, 12, 14)}},
                                                (9, 11, 13)), 1, (2, 0, 2, 2), NOTCLEAN, [], {8: ['issued=2']}),
    Spec('quit-rsx', 'W', 'quit@4', ok_markers('W', {10: 'quit', 12: 'skipped:quit', 14: 'skipped:quit'}, (11, 13)), 1,
         (3, 1, 3, 3), NOTCLEAN, [], {10: ['issued=1']}),
    Spec('quit-p1', 'W', 'quit@6', ok_markers('W', {14: 'quit'}), 1, (5, 2, 5, 5), NOTCLEAN, [], {14: ['issued=1']}),
    Spec('w0-quit-p0', 'W0', 'quit@1', ok_markers('W0', {8: 'quit', 10: 'skipped:quit', 12: 'skipped:variant-w0',
                                                         14: 'skipped:quit'}, (9, 13)), 1, (1, 0, 2, 2), NOTCLEAN),
    Spec('cp1-fflush', 'W', 'csv.fflush@1=00000005', ok_markers('W', {**skips('csv-failed', 10), 17: 'error'},
                                                                (9, 11, 13)), 1, (3, 0, 1, 0), NOTCLEAN, [],
         {8: ['csv=fflush:00000005'], 17: [f'fflush={NA} fsync={NA} fclose=ok']}),
    Spec('cp2-fsync', 'W', 'csv.fsync@2=00000005', ok_markers('W', {**skips('csv-failed', 12), 17: 'error'}, (11, 13)),
         1, (3, 2, 2, 2), NOTCLEAN, [], {10: ['csv=fsync:00000005'], 17: [f'fflush={NA}']}),
    Spec('cp3-fflush', 'W', 'csv.fflush@3=00000005', ok_markers('W', {14: 'skipped:csv-failed', 17: 'error'}, (13,)), 1,
         (4, 2, 3, 2), NOTCLEAN, [], {12: ['csv=fflush:00000005']}),
    Spec('cp4-fsync', 'W', 'csv.fsync@4=00000005', ok_markers('W', {17: 'error'}), 1, (7, 2, 4, 4), NOTCLEAN, [],
         {14: ['csv=fsync:00000005'], 17: [f'fflush={NA} fsync={NA} fclose=ok']}),
    Spec('final-fflush', 'W', 'csv.fflush@5=00000005', ok_markers('W', {17: 'error'}), 1, (7, 2, 5, 4), COMPLETE, [],
         {17: [f'fflush=00000005 fsync={NA} fclose=ok']}),
    Spec('final-fsync', 'W', 'csv.fsync@5=00000005', ok_markers('W', {17: 'error'}), 1, (7, 2, 5, 5), COMPLETE, [],
         {17: ['fflush=ok fsync=00000005 fclose=ok']}),
    Spec('final-fclose', 'W', 'csv.fclose=00000005', ok_markers('W', {17: 'error'}), 1, (7, 2, 5, 5), COMPLETE, [],
         {17: ['fclose=00000005']}),
    Spec('row-write-mid', 'W', 'csv.fprintf@4=fail', ok_markers('W', {8: 'csv-stopped', **skips('csv-failed', 10),
                                                                      17: 'error'}, (9, 11, 13)), 1, (2, 0, 0, 0),
         INVALIDV('end row counts'), [], {8: ['issued=2', f'csv={NA}']}),
    Spec('row-write-last', 'W', 'csv.fprintf@5=fail', ok_markers('W', {**skips('csv-failed', 10), 17: 'error'},
                                                                 (9, 11, 13)), 1, (3, 0, 0, 0), INVALIDV('end row counts'),
         [], {8: ['issued=3', 'csv=row-write']}),
    Spec('csv-open', 'W', 'csv.open=00000011', {1: 'ok', 2: 'error:open:00000011', 4: 'skipped:csv-failed',
                                                **skips('csv-failed', 6), 16: 'skipped:csv-failed',
                                                17: 'skipped:csv-failed'}, 1, (0, 0, 0, 0), None, csv=0),
    Spec('csv-fdopen', 'W', 'csv.fdopen=00000016', {1: 'ok', 2: 'error:fdopen:00000016', 4: 'skipped:csv-failed',
                                                    **skips('csv-failed', 6), 16: 'skipped:csv-failed',
                                                    17: 'skipped:csv-failed'}, 1, (0, 0, 0, 0),
         INVALIDV('empty file'), [], {2: ['rawclose=ok']}),
    Spec('csv-rawclose-fail', 'W', 'csv.fdopen=00000016,csv.rawclose=00000009',
         {1: 'ok', 2: 'error:fdopen:00000016', 4: 'skipped:csv-failed', **skips('csv-failed', 6),
          16: 'skipped:csv-failed', 17: 'skipped:csv-failed'}, 1, (0, 0, 0, 0), INVALIDV('empty file'), [],
         {2: ['rawclose=00000009']}),
    Spec('cleanup-terminate', 'W', f'cleanup.terminate={H}', ok_markers('W', {16: f'error:terminate:{H}'}), 1,
         (7, 2, 5, 5), NOTCLEAN, [], {16: ['left_running=1 spu_join=not-attempted']}),
    Spec('cleanup-join', 'W', f'cleanup.join={H}', ok_markers('W', {16: f'error:join:{H}'}), 1, (7, 2, 5, 5), NOTCLEAN,
         [], {16: ['spu_join=failed']}),
    Spec('cleanup-destroy', 'W', f'cleanup.destroy={H}', ok_markers('W', {16: f'error:destroy:{H}'}), 1, (7, 2, 5, 5),
         NOTCLEAN, ['SPU_PRODUCER_QUIESCED'], {16: ['left_running=0 spu_join=ok']}),
    Spec('cleanup-image', 'W', f'cleanup.image-close={H}', ok_markers('W', {16: f'error:image-close:{H}'}), 1,
         (7, 2, 5, 5), NOTCLEAN),
    Spec('tb-equal', 'W', 'tbstep=0,label=null', ok_markers('W', {10: 'unavailable:label-address'}), 0, (7, 0, 5, 5),
         INVALIDV('line 3: bracket tb1 <= tb0')),
]
for call in ('sysutil-register', 'init', 'import', 'group-create', 'thread-init', 'group-start'):
    SP.append(Spec(f'setup-{call}', 'W', f'setup.{call}={H}', {1: 'ok', 2: 'ok', 3: 'ok', 4: f'error:{call}:{H}',
                                                               **skips('spu-setup-failed', 6), 15: 'ok', 16: 'ok',
                                                               17: 'ok'}, 1, (0, 0, 1, 1), NOTCLEAN, [],
                   {16: ['spu_join=not-started']}))
SP.append(Spec('w0-setup-init', 'W0', f'setup.init={H}', {1: 'ok', 2: 'ok', 3: 'ok', 4: f'error:init:{H}',
                                                          **skips('spu-setup-failed', 6), 15: 'ok', 16: 'ok', 17: 'ok'},
               1, (0, 0, 1, 1), NOTCLEAN))
# logging failures: the attempted markers are the normal ones before F, then F, then ONE 99
LOGCNT = {'W': lambda F: (0, 0, 2, 2) if F <= 7 else (3, 0, 2, 2) if F == 8 else (3, 0, 3, 3) if F == 9 else
          (3, 2, 3, 3) if F in (10, 11) else (4, 2, 4, 4) if F == 12 else (4, 2, 5, 5) if F == 13 else (7, 2, 5, 5),
          'W0': lambda F: (0, 0, 2, 2) if F <= 7 else (3, 0, 2, 2) if F == 8 else (3, 0, 3, 3) if F == 9 else
          (3, 2, 3, 3) if F in (10, 12) else (3, 2, 4, 4) if F == 13 else (6, 2, 4, 4)}


def log_spec(name, var, F, faults, kind, has99=True):
    norm = ok_markers(var, {12: 'skipped:variant-w0'} if var == 'W0' else {})
    m = {s: r for s, r in norm.items() if s < F}
    if kind != 'open':
        m[F] = norm[F]
    if has99:
        m[99] = 'failed'
    inv = {F} if kind in ('zero', 'partial') else set()
    facts = [f'LOGGING_FAILURE {F:02d}'] if has99 and not inv else []
    return Spec(name, var, faults, m, 1, LOGCNT[var](F), COMPLETE if F >= 16 else NOTCLEAN, facts,
                {99: [f'prev={F:02d}:']} if has99 else {}, inv)


for var in ('W', 'W0'):
    for F in FULL[var]:
        SP.append(log_spec(f'log-open-{var}-{F:02d}', var, F, f'mk{F:02d}.open=00000005', 'open'))
for kind, key, err in (('zero', 'mk08.write=zero', 'error:write:zero'),
                       ('partial', 'mk08.write=part:20:00000005', 'error:write:00000005'),
                       ('fsync', 'mk08.fsync=00000005', 'error:fsync:00000005'),
                       ('close', 'mk08.close=00000005', 'error:close:00000005')):
    s = log_spec(f'log-{kind}-08', 'W', 8, key, kind)
    s.fields = {99: [f'prev=08:{err}:']}
    SP.append(s)
SP.append(log_spec('log-99-fails', 'W', 8, 'mk08.open=00000005,mk99.open=00000005', 'open', has99=False))


def markers_in(d):
    res = {}
    for p in sorted(d.glob('*.mk')):
        b = p.read_bytes()
        m = re.search(rb'result=(\S+) ', b)
        res[int(p.name[-5:-3])] = (m.group(1).decode() if m else '<unparsable>', b.decode(errors='replace'))
    return res


def norm_trace(t):
    o = []
    for l in t.splitlines():
        m = re.match(r'mk-write (\d\d) (\d+) -> (.*)$', l)
        if m:
            l = f'mk-write {m.group(1)} L -> L' if m.group(3) == m.group(2) else f'mk-write {m.group(1)} N -> {m.group(3)}'
        o.append(l)
    return o


def mk_structure(trace):
    """the attempted markers in order, or an error: each open, then (if it opened) >= 1 write, at most one fsync and
    exactly one close for THAT marker, before the next open; a failed open has nothing after it"""
    seqs, cur, state = [], None, None
    for l in trace.splitlines():
        m = re.match(r'mk-(open|write|fsync|close) (\d\d) (.*)$', l)
        if not m:
            continue
        op, s, rest = m.group(1), int(m.group(2)), m.group(3)
        if op == 'open':
            if cur is not None and state not in ('closed', 'failed'):
                return None, f'marker {cur:02d} not closed before {s:02d} opened'
            cur, state = s, 'open' if rest == '-> fd' else 'failed'
            seqs.append(s)
            continue
        if s != cur or state in ('closed', 'failed'):
            return None, f'{op} of {s:02d} outside its open/close bracket'
        if op == 'close':
            state = 'closed'
    if cur is not None and state not in ('closed', 'failed'):
        return None, f'marker {cur:02d} never closed'
    return seqs, None


def counters(trace):
    t = trace.splitlines()
    c = lambda p: sum(1 for l in t if l.startswith(p))  # noqa: E731
    return (c('spu-mb '), c('rsx-flush '), c('csv-fflush '), c('csv-fsync ')), c('csv-fclose'), c('csv-rawclose'), \
        c('csv-open')


def judge(sp, d, trace, rc, stderr, analyzer=None):
    why = []
    got = markers_in(d)
    if sorted(got) != sorted(sp.markers):
        why.append(f'marker files {sorted(got)} != pinned {sorted(sp.markers)}')
    for s, res in sp.markers.items():
        if s in got and s not in sp.invalid and got[s][0] != res:
            why.append(f'{s:02d}: {got[s][0]} != {res}')
    for s, subs in sp.fields.items():
        for sub in subs:
            if sub not in got.get(s, ('', ''))[1]:
                why.append(f'{s:02d} lacks {sub!r}')
    attempted = sorted(s for s in sp.markers) if sp.name != 'log-99-fails' else sorted(sp.markers) + [99]
    if sp.name.startswith('log-open') or sp.name == 'log-99-fails':
        F = int(re.search(r'mk(\d\d)\.open', sp.faults).group(1))
        attempted = sorted(set(s for s in sp.markers if s != 99) | {F}) + [99]
    seqs, err = mk_structure(trace)
    if err:
        why.append(err)
    elif seqs != attempted:
        why.append(f'attempted markers {seqs} != {attempted}')
    cnt, fclose, raw, copen = counters(trace)
    if cnt != sp.cnt:
        why.append(f'requests/checkpoints (spu, rsx, fflush, fsync) {cnt} != pinned {sp.cnt}')
    fd_fail = 'csv.fdopen' in sp.faults
    if (fclose, raw) != ((0, 1) if fd_fail else (sp.csv, 0)):
        why.append(f'fclose {fclose} / raw close {raw} unexpected')
    if rc != sp.rc:
        why.append(f'rc {rc} != {sp.rc}')
    if stderr:
        why.append('stderr: ' + stderr[:160])
    vrc, vo = validate(d)
    want_vrc = 1 if sp.invalid else 0
    if vrc != want_vrc:
        why.append(f'validator rc {vrc} != {want_vrc}: '
                   f'{[l for l in vo.splitlines() if l.startswith(("INVALID", "INCONSISTENT"))][:2]}')
    for f in sp.facts:
        if f not in vo:
            why.append(f'fact {f!r} missing')
    csvs = sorted(d.glob('*.csv'))
    want_csv = 0 if sp.analyzer is None else 1
    if len(csvs) != want_csv:
        why.append(f'{len(csvs)} CSV file(s), pinned {want_csv}')
    if sp.analyzer is not None and csvs:
        if analyzer is None:
            a = subprocess.run([sys.executable, '-I', str(ANALYZE), str(csvs[0])], capture_output=True, text=True)
            analyzer = (a.returncode, a.stdout, a.stderr)
        arc, aout, aerr = analyzer
        verdict = next((l for l in aout.splitlines() if l.startswith('VERDICT')), None)
        if aerr.strip() or verdict is None or arc != sp.analyzer[0] or not verdict.startswith(sp.analyzer[1]):
            why.append(f'analyzer rc {arc} verdict {verdict!r} (stderr {aerr.strip()[:60]!r}) != pinned {sp.analyzer}')
    return why


snap = {}
for var in ('W', 'W0'):
    for san, extra in (('plain', []), ('asan', SAN)):
        for sp in SP:
            if sp.var != var:
                continue
            d = out / f'{sp.name}-{san}'
            d.mkdir()
            exe = build(f'{sp.name}-{san}', d, var, extra)
            if not exe:
                row(f'{sp.name} {san}', False, 'build failed')
                continue
            tr = out / f'{sp.name}-{san}.trace'
            r = subprocess.run([str(exe)], capture_output=True, text=True,
                               env=dict(os.environ, W_FAULTS=sp.faults, W_TRACE=str(tr)), timeout=120)
            trace = tr.read_text() if tr.exists() else ''
            why = judge(sp, d, trace, r.returncode, r.stderr)
            snap[f'{sp.name}-{san}'] = (sp, d, trace, r.returncode, r.stderr)
            row(f'{sp.name} {san}', not why, '; '.join(why))
# neither full path fits: no CSV open and no marker call at all, rc 1
d = out / ('p' * 520)
exe = build('longpath', d, 'W', ['-Wno-format-truncation'])
if exe:
    tr = out / 'longpath.trace'
    r = subprocess.run([str(exe)], capture_output=True, text=True, env=dict(os.environ, W_TRACE=str(tr)), timeout=60)
    t = tr.read_text() if tr.exists() else ''
    row('format/path: zero CSV opens, zero marker calls, rc 1', r.returncode == 1 and 'csv-open' not in t and
        'mk-' not in t and not os.path.exists(str(d)), f'rc {r.returncode}')
else:
    row('format/path', False, 'build failed')

# ================================================================ J. judge controls (each must fail, for its reason)
def control(name, key, mutate, reason, analyzer=None):
    if key not in snap:
        row(f'judge control {name}', False, f'no clean {key} run')
        return
    sp, d, trace, rc, se = snap[key]
    cd = out / f'jc-{name}'
    shutil.copytree(d, cd)
    trace = mutate(cd, trace)
    why = judge(sp, cd, trace, rc, se, analyzer)
    row(f'judge control {name}', any(reason in w for w in why), f'rejected for: {why[:2]}')


control('earlier-marker-deleted', 'log-open-W-08-plain', lambda d, t: (next(d.glob('*-05.mk')).unlink(), t)[1],
        'marker files')
control('required-csv-deleted', 'normal-plain', lambda d, t: (next(d.glob('*.csv')).unlink(), t)[1], 'CSV file')
control('extra-request-after-stop', 'p0-timeout-plain', lambda d, t: t + 'spu-mb 9 seq 00000009 -> 0\n',
        'requests/checkpoints')
control('analyzer-crashed', 'normal-plain', lambda d, t: t, 'analyzer',
        analyzer=(1, '', 'Traceback (most recent call last):\n  ...\nKeyError: x'))
control('analyzer-empty', 'normal-plain', lambda d, t: t, 'analyzer', analyzer=(0, '', ''))

# ================================================================ T. exact traces
def mk(s):
    return [f'mk-open {s:02d} -> fd', f'mk-write {s:02d} L -> L', f'mk-fsync {s:02d} -> 0', f'mk-close {s:02d} -> 0']


def spu(a, b):
    return [f'spu-mb {k} seq {k:08x} -> 0' for k in range(a, b + 1)]


def csvcp(k):
    return [f'csv-fflush {k} -> 0', f'csv-fsync {k} -> 0']


CLEAN = ['cleanup terminate -> 00000000', 'cleanup join -> 00000000', 'cleanup destroy -> 00000000',
         'cleanup image-close -> 00000000']
EXACT = {
    'normal-plain': mk(1) + ['csv-open -> fd', 'csv-fdopen -> FILE'] + mk(2) + mk(3) + mk(4) + mk(5) + mk(6) + mk(7) +
    spu(1, 3) + csvcp(1) + mk(8) + mk(9) + ['rsx-flush 1 want 00000001', 'rsx-flush 2 want 00000002'] + csvcp(2) +
    mk(10) + mk(11) + ['spu-mb 4 seq 80000004 -> 0'] + csvcp(3) + mk(12) + mk(13) +
    ['spu-mb 5 seq 00000005 -> 0', 'spu-mb 6 seq 00000006 -> 0', 'spu-mb 7 seq 00000007 -> 0'] + csvcp(4) + mk(14) +
    mk(15) + CLEAN + mk(16) + ['csv-fflush 5 -> 0', 'csv-fsync 5 -> 0', 'csv-fclose -> 0'] + mk(17),
    'log-open-W-08-plain': mk(1) + ['csv-open -> fd', 'csv-fdopen -> FILE'] + mk(2) + mk(3) + mk(4) + mk(5) + mk(6) +
    mk(7) + spu(1, 3) + csvcp(1) + ['mk-open 08 -> -1 00000005'] + mk(99) + CLEAN +
    ['csv-fflush 2 -> 0', 'csv-fsync 2 -> 0', 'csv-fclose -> 0'],
    'log-partial-08-plain': mk(1) + ['csv-open -> fd', 'csv-fdopen -> FILE'] + mk(2) + mk(3) + mk(4) + mk(5) + mk(6) +
    mk(7) + spu(1, 3) + csvcp(1) + ['mk-open 08 -> fd', 'mk-write 08 N -> 20', 'mk-write 08 N -> -1 00000005',
                                     'mk-close 08 -> 0'] + mk(99) + CLEAN +
    ['csv-fflush 2 -> 0', 'csv-fsync 2 -> 0', 'csv-fclose -> 0'],
    'cp1-fflush-plain': mk(1) + ['csv-open -> fd', 'csv-fdopen -> FILE'] + mk(2) + mk(3) + mk(4) + mk(5) + mk(6) +
    mk(7) + spu(1, 3) + ['csv-fflush 1 -> EOF 00000005'] + mk(8) + mk(10) + mk(12) + mk(14) + mk(15) + CLEAN + mk(16) +
    ['csv-fclose -> 0'] + mk(17),
    'csv-fdopen-plain': mk(1) + ['csv-open -> fd', 'csv-fdopen -> NULL 00000016', 'csv-rawclose -> 0'] + mk(2) + mk(4) +
    mk(6) + mk(8) + mk(10) + mk(12) + mk(14) + mk(16) + mk(17),
}
for name, want_t in EXACT.items():
    t = out / f'{name}.trace'
    got_t = norm_trace(t.read_text()) if t.exists() else []
    k = next((i for i, (x, y) in enumerate(zip(got_t, want_t)) if x != y), None)
    row(f'exact trace {name}', got_t == want_t, '' if got_t == want_t else
        f'first difference at {k}: {got_t[k:k + 2] if k is not None else got_t[len(want_t):len(want_t) + 2]} vs '
        f'{want_t[k:k + 2] if k is not None else want_t[len(got_t):len(got_t) + 2]} ({len(got_t)} vs {len(want_t)})')

# ================================================================ B. the build tag bound
for idv, ok in (('0123456789abcdef', True), ('0123456789abcde', False), ('0123456789abcdef0', False),
                ('0123456789ABCDEF', False), ('', False)):
    r = subprocess.run(['cmake', f'-DW_BUILD_ID={idv}', '-P', str(top / 'w_build_id.cmake')], capture_output=True, text=True)
    row(f'cmake id {idv or "(empty)"}', (r.returncode == 0) == ok, f'rc {r.returncode}')
for idv, ok in (('0123456789abcdef', True), ('0123456789abcde', False), ('0123456789abcdef0', False)):
    r = subprocess.run([cc, '-std=c11', '-fsyntax-only', '-DW_HOST_TEST', f'-DW_DIR="{out}"', f'-DW_BUILD_ID="{idv}"',
                        f'-I{here}', f'-I{top.parent / "clock-calibration/source"}', str(top / 'source/main.c')],
                       capture_output=True, text=True)
    row(f'producer id length {len(idv)}', (r.returncode == 0) == ok)
    if not ok:
        vrc, _ = validate(md, build=idv)
        row(f'validator --build length {len(idv)}', vrc == 2, f'rc {vrc}')

bad = [n for n, ok in rows if not ok]
print(f'W HOST {"PASS" if not bad else "FAIL"}: {len(rows)} rows, {len(bad)} failed {bad if bad else ""}'.rstrip())
sys.exit(1 if bad else 0)
