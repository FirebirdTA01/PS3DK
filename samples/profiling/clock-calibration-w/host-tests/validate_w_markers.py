#!/usr/bin/env python3
"""Validate W / W0 probe markers (CLKW1, design r4 2afa7bf3 + review 19:47 / 20:15) fetched from /dev_hdd0/tmp.

usage: validate_w_markers.py DIR --build BUILD [--run RUN] [--targets SPU,RSX]

A RECORD is VALID only if the whole file is exactly one CLKW1 line whose stage/result/fields are a row of the PINNED
MATRIX (MATRIX.md; anything not listed is FORBIDDEN), with run/seq equal to the file name, build == --build, len ==
the byte length of the prefix before " len=", numbers within their widths and the build's bounds (--targets =
CAL_SPU_SAMPLES,CAL_RSX_SAMPLES, default 1500,1500), a predecessor the producer can have named (PRED), and unres
consistent with its own result.

CROSS-RECORD rules per run, over the PRESENT records (absent records are UNKNOWN, never a violation by themselves;
contradictions between present records are): one variant; the predecessor is the immediately preceding PRESENT
marker (no present file lies between them); its tb <= its completion time <= this tb (nondecreasing); unres sticky;
counts nondecreasing; nothing normal after the marker a valid 99 names; and the TRANSITIONS: replaying the present
records through the producer's skip rules (csv-failed / spu-setup-failed, then variant-w0, then gcm-unavailable,
then spu-stopped, then quit), every later present record must be what the producer writes in that state.

FACTS (VALID records of a CONSISTENT run): REACHED; UNRESOLVED_SUBMISSION spu|rsx (historical); SUBMISSION_OUTCOME_
UNKNOWN spu; SUBMISSION_REFUSED spu (producer-classified); SPU_PRODUCER_QUIESCED; WRITE_REQUEST_REPLY; LOGGING_FAILURE;
PROCESS_EXIT=UNOBSERVED.  For an INCONSISTENT run only REACHED and PROCESS_EXIT=UNOBSERVED are printed (the derived
facts are WITHHELD).  Exit 0: every considered record VALID and every run consistent; 1: otherwise; 2: usage or
nothing considered."""
import argparse
from pathlib import Path
import re
import sys

H8 = r'[0-9a-f]{8}'
DEC = r'(?:0|[1-9][0-9]{0,9})'
U32 = 4294967295
IO = rf'(?:ok|error:(?:open|write|fsync|close):{H8}|error:write:zero|error:format:(?:record|path))'
CSV = rf'(?:ok|row-write|fflush:{H8}|fsync:{H8}|not-attempted)'
X3 = rf'(?:ok|{H8}|not-attempted)'
BEGIN = {3: 'spu-setup-begin', 5: 'gcm-setup-begin', 7: 'phase0-begin', 9: 'rsx-begin', 11: 'write-req-begin',
         13: 'phase1-begin', 15: 'teardown-begin'}
STAGES = {1: 'main-entered', 2: 'csv-opened', 4: 'spu-setup-done', 6: 'gcm-setup-done', 8: 'phase0-done',
          10: 'rsx-done', 12: 'write-req-done', 14: 'phase1-done', 16: 'teardown-done', 17: 'csv-closed',
          99: 'logging-failed', **BEGIN}
SPU_PHASE = rf'ok|timeout|identity|mbox-refused:{H8}|mbox-error:{H8}|quit|csv-stopped'
PH_F = rf'issued={DEC}(?: last={H8})? csv={CSV}'
MATRIX = {
    1: {'*': (r'ok', r'')},
    2: {'*': (rf'ok|error:open:{H8}|error:fdopen:{H8}|error:format:path', rf'(?:rawclose=(?:ok|{H8}))?')},
    4: {'*': (rf'ok|error:(?:sysutil-register|init|import|group-create|thread-init|group-start):{H8}|'
              r'skipped:csv-failed', r'')},
    6: {'*': (rf'ok|unavailable:(?:host-buffer|gcm-init):{H8}|skipped:(?:csv-failed|spu-setup-failed)', r'')},
    8: {'*': (rf'{SPU_PHASE}|skipped:(?:csv-failed|spu-setup-failed)', PH_F)},
    10: {'*': (r'ok|timeout|identity|quit|csv-stopped|unavailable:label-address|'
               r'skipped:(?:csv-failed|spu-setup-failed|gcm-unavailable|quit)', PH_F)},
    12: {'W': (rf'ok|timeout|identity|mbox-refused:{H8}|mbox-error:{H8}|'
               r'skipped:(?:csv-failed|spu-setup-failed|spu-stopped|quit)', rf'(?:dec={H8} dec2={H8} wrote={H8} )?csv={CSV}'),
         'W0': (r'skipped:(?:csv-failed|spu-setup-failed|variant-w0)', r'csv=not-attempted')},
    14: {'*': (rf'{SPU_PHASE}|skipped:(?:csv-failed|spu-setup-failed|spu-stopped|quit)', PH_F)},
    16: {'*': (rf'ok|error:(?:terminate|join|destroy|image-close):{H8}|skipped:csv-failed',
               r'left_running=[01] spu_join=(?:ok|failed|not-attempted|not-started)')},
    17: {'*': (r'ok|error|skipped:csv-failed', rf'fflush={X3} fsync={X3} fclose={X3}')},
    99: {'*': (r'failed', r'')},
    **{s: {'*': (r'ok', r'')} for s in BEGIN if s != 11},
    11: {'W': (r'ok', r'')},
}
# the predecessors the producer can name (normal records); 99 names any attempted milestone of its variant
PRED = {2: {1}, 3: {2}, 4: {3, 2}, 5: {4}, 6: {5, 4}, 7: {6}, 8: {7, 6}, 9: {8}, 10: {9, 8}, 11: {10},
        12: {11, 10}, 13: {12}, 14: {13, 12}, 15: {14}, 16: {15, 14}, 17: {16}}
REC = re.compile(rb'CLKW1 run=([0-9a-f]{16}) variant=(W|W0) build=([0-9a-f]{16}) seq=([0-9]{2}) stage=([a-z0-9-]+) '
                 rb'result=(\S+) tb=([0-9a-f]{16}) prev=(none|([0-9]{2}):' + IO.encode() + rb':([0-9a-f]{16})) '
                 rb'unres=([nuq]),([nu]) counts=(' + DEC.encode() + rb'),(' + DEC.encode() + rb')((?: \S+)*)')
NAME = re.compile(r'clockcal-([0-9a-f]{16})-([0-9]{2})\.mk')
STOPS = ('timeout', 'identity', 'mbox-error:', 'mbox-refused:')


class Bad(Exception):
    pass


def fields(s):
    return dict(f.split('=', 1) for f in s.split())


def check_record(data, run, seq_s, build, targets):
    T, R = targets
    i = data.rfind(b' len=')
    if i < 0:
        raise Bad('no len field')
    prefix, tail = data[:i], data[i:]
    t = re.fullmatch(rb' len=(0|[1-9][0-9]{0,3})\n', tail)
    if not t:
        raise Bad('bad len field or terminator')
    if int(t.group(1)) != len(prefix):
        raise Bad(f'len {int(t.group(1))} != prefix {len(prefix)}')
    m = REC.fullmatch(prefix)
    if not m:
        raise Bad('record does not parse')
    (r_run, var, b, seq, stage, result, tb, prev, pseq, pdone, us, ur, cs, cr, fl) = \
        (g.decode() if g is not None else None for g in m.groups())
    seq_i = int(seq)
    if r_run != run or seq != seq_s:
        raise Bad('run/seq differ from the file name')
    if b != build:
        raise Bad(f'build {b} != {build}')
    if STAGES.get(seq_i) != stage:
        raise Bad(f'unknown milestone {seq} {stage}')
    row = MATRIX[seq_i].get(var) or MATRIX[seq_i].get('*')
    if row is None:
        raise Bad(f'stage {seq} not possible for variant {var}')
    if not re.fullmatch(row[0], result):
        raise Bad(f'stage {seq} cannot carry result {result}')
    fl = fl.strip()
    if not re.fullmatch(row[1], fl):
        raise Bad(f'stage {seq} fields {fl!r} do not match the matrix')
    f = fields(fl) if fl else {}
    counts = (int(cs), int(cr))
    if counts[0] > min(U32, 2 * T) or counts[1] > min(U32, R):
        raise Bad(f'counts {counts} beyond the u32 width or the build bounds {2 * T},{R}')
    # the predecessor
    if seq_i == 1:
        if prev != 'none':
            raise Bad('01 must have prev=none')
    elif prev == 'none':
        raise Bad('only 01 has prev=none')
    else:
        p = int(pseq)
        ok_io = prev.split(':')[1] == 'ok'
        if seq_i == 99:
            if ok_io:
                raise Bad('99 must name a failed predecessor')
            if p not in STAGES or p == 99 or (p == 11 and var == 'W0'):
                raise Bad(f'99 names {pseq}, not an attempted milestone of {var}')
        else:
            if not ok_io:
                raise Bad('a normal record needs a successful predecessor')
            allowed = PRED[seq_i] - ({11} if var == 'W0' else set())
            if p not in allowed:
                raise Bad(f'{seq} cannot follow {pseq} (the producer names one of {sorted(allowed)})')
    if f.get('rawclose') is not None and not result.startswith('error:fdopen'):
        raise Bad('rawclose only after an fdopen failure')
    if result.startswith('error:fdopen') and 'rawclose' not in f:
        raise Bad('an fdopen failure records the raw close')
    # unres: before any request n,n; and consistent with this record's own result
    if seq_i < 8 and (us, ur) != ('n', 'n'):
        raise Bad('unres before any request was issued')
    if seq_i < 10 and ur != 'n':
        raise Bad('rsx unres before the RSX phase')
    if seq_i in (8, 12, 14):
        want_s = {'timeout': 'u', 'identity': 'u'}.get(result) or ('q' if result.startswith('mbox-error') else None)
        if want_s and us != want_s:
            raise Bad(f'result {result} needs SPU unres {want_s}, not {us}')
        if result.startswith('mbox-refused') and us != 'n':
            raise Bad('a classified refusal invents no SPU unres')
        if not result.startswith('skipped') and not want_s and not result.startswith('mbox-refused') and us != 'n':
            raise Bad(f'{result} at {seq}: the SPU domain was not stopped before, unres must be n')
    if seq_i == 10:
        if result in ('timeout', 'identity') and ur != 'u':
            raise Bad(f'result {result} needs RSX unres u')
        if not result.startswith('skipped') and result not in ('timeout', 'identity') and ur != 'n':
            raise Bad(f'{result} at 10: RSX unres must be n')
    skipped = result.startswith('skipped:')
    if seq_i in (8, 10, 14):
        n = int(f['issued'])
        tgt = R if seq_i == 10 else T
        if n > tgt:
            raise Bad(f'issued {n} beyond the target {tgt}')
        if ('last' in f) != (n >= 1):
            raise Bad('last present iff issued >= 1')
        if 'last' in f:
            want_last = n if seq_i in (8, 10) else T + n + (1 if var == 'W' else 0)
            if int(f['last'], 16) != want_last:
                raise Bad(f'last {int(f["last"], 16)} != {want_last} for issued {n}')
        if skipped and (n or f['csv'] != 'not-attempted'):
            raise Bad('a skipped phase has issued=0 and csv=not-attempted')
        if result == 'ok' and n != tgt:
            raise Bad(f'ok needs issued == {tgt}')
        if result in ('timeout', 'identity') and n < 1:
            raise Bad('a timeout/identity follows a submitted request')
        if result in ('quit', 'csv-stopped') and n >= tgt:
            raise Bad('quit/csv-stopped needs issued < the target')
        if (result.startswith('mbox-') or result.startswith('unavailable')) and n >= tgt:
            raise Bad('a refused/unavailable phase cannot have issued the target')
        if result == 'unavailable:label-address' and n:
            raise Bad('label-address unavailable: nothing issued')
        if result == 'csv-stopped' and f['csv'] != 'not-attempted':
            raise Bad('csv-stopped: the checkpoint is not attempted')
        if seq_i == 8 and counts[0] != n - (1 if result in ('timeout', 'identity') else 0):
            raise Bad(f'08: SPU count {counts[0]} vs {n} issued')
    if seq_i == 12:
        if ('dec' in f) != (result == 'ok'):
            raise Bad('dec/dec2/wrote present iff ok')
        if result == 'ok' and f['wrote'] != 'ffffffff':
            raise Bad('wrote must be the written value')
        if skipped and f['csv'] != 'not-attempted':
            raise Bad('a skipped 12 has csv=not-attempted')
    if seq_i == 16:
        lr, j = f['left_running'], f['spu_join']
        if skipped and (lr, j) != ('0', 'not-started'):
            raise Bad('a skipped teardown: left_running=0 spu_join=not-started')
        if result.startswith('error:terminate') and (lr, j) != ('1', 'not-attempted'):
            raise Bad('terminate failed: left_running=1 spu_join=not-attempted')
        if result.startswith('error:join') and (lr, j) != ('1', 'failed'):
            raise Bad('join failed: left_running=1 spu_join=failed')
        if (result == 'ok' or result.startswith('error:destroy') or result.startswith('error:image-close')) and \
                (lr != '0' or j not in ('ok', 'not-started')):
            raise Bad('after a completed terminate/join step: left_running=0, spu_join ok or not-started')
    if seq_i == 17:
        a, b2, c = f['fflush'], f['fsync'], f['fclose']
        if skipped and (a, b2, c) != ('not-attempted',) * 3:
            raise Bad('a skipped close attempts nothing')
        if not skipped and c == 'not-attempted':
            raise Bad('fclose is attempted once whenever a FILE exists')
        if a != 'ok' and b2 != 'not-attempted':
            raise Bad('fsync only after a successful fflush')
        if result == 'ok' and (a, b2, c) != ('ok', 'ok', 'ok'):
            raise Bad('ok needs fflush, fsync and fclose ok')
    return {'seq': seq_i, 'stage': stage, 'result': result, 'tb': int(tb, 16), 'pseq': int(pseq) if pseq else None,
            'pdone': int(pdone, 16) if pdone else None, 'perr': prev.split(':', 1)[1].rsplit(':', 1)[0] if pseq else None,
            'unres': (us, ur), 'counts': counts, 'f': f, 'var': var}


def transitions(recs):
    """replay the present VALID records through the producer's skip rules; return the contradictions"""
    bad = []
    st = {'csv_open': False, 'setup': False, 'csv': False, 'gcm': False, 'spu': False, 'quit': False}
    var = recs[0]['var'] if recs else 'W'
    for r in recs:
        s, res = r['seq'], r['result']
        if s == 99:
            continue
        cause = 'csv-failed' if st['csv_open'] or st['csv'] else 'spu-setup-failed' if st['setup'] else None
        want = None
        if s in (4, 6, 16, 17) and st['csv_open']:
            want = 'skipped:csv-failed'
        elif s == 6 and st['setup']:
            want = 'skipped:spu-setup-failed'
        elif s in (8, 10, 12, 14):
            if s == 8 and (st['csv_open'] or st['setup']):
                want = f'skipped:{cause}'
            elif s != 8 and cause:
                want = f'skipped:{cause}'
            elif s == 12 and var == 'W0':
                want = 'skipped:variant-w0'
            elif s == 10 and st['gcm']:
                want = 'skipped:gcm-unavailable'
            elif s in (12, 14) and st['spu']:
                want = 'skipped:spu-stopped'
            elif s != 8 and st['quit']:
                want = 'skipped:quit'
        begin_blocked = s in BEGIN and s != 15 and (
            st['csv_open'] or (s != 3 and st['setup']) or (s in (9, 11, 13) and (st['csv'] or st['quit'])) or
            (s == 9 and st['gcm']) or (s in (11, 13) and st['spu']) or (s == 11 and var == 'W0'))
        if begin_blocked:
            bad.append(f'{s:02d} {r["stage"]} present although an earlier present record stopped that stage')
        if s == 15 and st['csv_open']:
            bad.append('15 present after a CSV open failure')
        if want and res != want:
            bad.append(f'{s:02d}: {res}, but the earlier present records require {want}')
        if s == 17 and st['csv'] and not st['csv_open'] and (r['f'].get('fflush'), r['f'].get('fsync')) != \
                ('not-attempted', 'not-attempted'):
            bad.append('17: final fflush/fsync attempted after an earlier CSV failure')
        # update the state from this record
        if s == 2 and res != 'ok':
            st['csv_open'] = True
        if s == 4 and res.startswith('error:'):
            st['setup'] = True
        if s == 6 and res.startswith('unavailable:'):
            st['gcm'] = True
        if res == 'csv-stopped' or r['f'].get('csv', 'ok') not in ('ok', 'not-attempted'):
            st['csv'] = True
        if s in (8, 12, 14) and res.startswith(STOPS):
            st['spu'] = True
        if s in (8, 10, 14) and res == 'quit':
            st['quit'] = True
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir', type=Path)
    ap.add_argument('--build', required=True)
    ap.add_argument('--run')
    ap.add_argument('--targets', default='1500,1500')
    a = ap.parse_args()
    if not re.fullmatch(r'[0-9a-f]{16}', a.build) or (a.run and not re.fullmatch(r'[0-9a-f]{16}', a.run)) or \
            not re.fullmatch(r'[1-9][0-9]{0,8},[1-9][0-9]{0,8}', a.targets) or not a.dir.is_dir():
        print('usage: --build/--run 16 lowercase hex, --targets N,M, DIR a directory')
        return 2
    targets = tuple(int(x) for x in a.targets.split(','))
    runs, present, bad, n = {}, {}, 0, 0
    for p in sorted(a.dir.iterdir()):
        m = NAME.fullmatch(p.name)
        if not m or not p.is_file():
            if p.name.endswith('.mk'):
                print(f'ignored {p.name}: name does not match')
            continue
        if a.run and m.group(1) != a.run:
            continue
        n += 1
        present.setdefault(m.group(1), set()).add(int(m.group(2)))
        try:
            r = check_record(p.read_bytes(), m.group(1), m.group(2), a.build, targets)
        except Bad as x:
            bad += 1
            print(f'INVALID {p.name}: {x}')
            continue
        runs.setdefault(m.group(1), []).append(r)
        print(f'VALID {p.name}: {r["stage"]} result={r["result"]}')
    for run, recs in sorted(runs.items()):
        recs.sort(key=lambda r: r['seq'])
        by = {r['seq']: r for r in recs}
        problems = []
        if len({r['var'] for r in recs}) != 1:
            problems.append('mixed variants')
        for r in recs:
            if r['pseq'] is None:
                continue
            between = sorted(s for s in present[run] if r['pseq'] < s < r['seq'] and s != 99)
            if between and r['seq'] != 99:
                problems.append(f'{r["seq"]:02d} names {r["pseq"]:02d} but present marker(s) {between} lie between')
            p = by.get(r['pseq'])
            if p and not (p['tb'] <= r['pdone'] <= r['tb']):
                problems.append(f'{r["seq"]:02d}: time order (nondecreasing) broken against {r["pseq"]:02d}')
            elif r['pdone'] > r['tb']:
                problems.append(f'{r["seq"]:02d}: prev completion after this record was formatted')
        if 99 in by:
            f99 = by[99]['pseq']
            if any(s > f99 and s != 99 for s in present[run]):
                problems.append('a normal record after the failed marker named by 99')
        for k in (0, 1):
            seen = 'n'
            for r in recs:
                u = r['unres'][k]
                if seen != 'n' and u != seen:
                    problems.append(f'unres[{k}] changed {seen} -> {u} at {r["seq"]:02d} (sticky)')
                seen = u if u != 'n' else seen
        last = (0, 0)
        for r in recs:
            if r['counts'][0] < last[0] or r['counts'][1] < last[1]:
                problems.append(f'counts decreased at {r["seq"]:02d}')
            last = r['counts']
        problems += transitions([r for r in recs if r['seq'] != 99])
        facts = ['REACHED ' + ' '.join(f'{r["seq"]:02d}' for r in recs)]
        if not problems:
            if any(r['unres'][0] == 'u' for r in recs):
                facts.append('UNRESOLVED_SUBMISSION spu')
            if any(r['unres'][1] == 'u' for r in recs):
                facts.append('UNRESOLVED_SUBMISSION rsx')
            if any(r['unres'][0] == 'q' for r in recs):
                facts.append('SUBMISSION_OUTCOME_UNKNOWN spu')
            if any(r['result'].startswith('mbox-refused') for r in recs):
                facts.append('SUBMISSION_REFUSED spu (producer-classified)')
            if 16 in by and by[16]['f'].get('spu_join') == 'ok':
                facts.append('SPU_PRODUCER_QUIESCED')
            if 12 in by and by[12]['result'] == 'ok':
                f = by[12]['f']
                facts.append(f'WRITE_REQUEST_REPLY dec={f["dec"]} dec2={f["dec2"]} wrote={f["wrote"]}')
            if 99 in by:
                facts.append(f'LOGGING_FAILURE {by[99]["pseq"]:02d} {by[99]["perr"]}')
        else:
            facts.append('derived facts WITHHELD (inconsistent run)')
        facts.append('PROCESS_EXIT=UNOBSERVED')
        for pr in problems:
            print(f'INCONSISTENT run {run}: {pr}')
        print(f'run {run}: ' + '; '.join(facts) + ' (missing records are unknown)')
        bad += len(problems)
    if n == 0:
        print('no marker file considered: nothing to state')
        return 2
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
