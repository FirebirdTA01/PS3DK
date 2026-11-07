#!/usr/bin/env python3
"""Validate E1 probe markers (EXTP1, design p3.3 E1 r2 section 1 + r3 section D + r4 section 2) from one run directory.

usage: validate_e1_markers.py DIR --build BUILD [--run RUN]

A RECORD is VALID only if the whole file is exactly one EXTP1 line whose stage/result/fields are a row of the MATRIX
below (anything not listed is FORBIDDEN), with run/seq equal to the file name, build == --build, len == the byte
length of the prefix before " len=", the line within 400 bytes, numbers within their widths, a predecessor the probe
can have named, and unres/counts consistent with its own result.

CROSS-RECORD rules over the PRESENT records (an absent record is UNKNOWN, never a violation by itself; contradictions
between present records are): one build; the predecessor is the immediately preceding PRESENT marker; time order
(its tb <= its completion <= this tb); unres sticky; counts nondecreasing; nothing normal after the failed marker a
valid 99 names; and the FLOW: each present record must be a successor the probe can write after the previous present
record's RESULT (SUCC), plus the state rules of the cleanup (r3 C): terminate only for a program that may still run,
join only after a successful terminate or for a program that exited, lane_end only after a successful join, the GPU
phase only after a clean SPU phase, idle/release only after a successful detach of complete frames, capture check only
after a successful shutdown.

FACTS (VALID records of a CONSISTENT run): REACHED; UNRESOLVED_SUBMISSION spu|rsx; SUBMISSION_OUTCOME_UNKNOWN spu;
SPU_EXITED (a trusted report says so); SPU_JOINED; READ_IDX (09) / SEEN_RIDX (12); GPU_FRAMES; LOGGING_FAILURE;
PROCESS_EXIT=UNOBSERVED.  For an INCONSISTENT run only REACHED and PROCESS_EXIT=UNOBSERVED are printed.  Exit 0: every
record VALID and the run consistent; 1: otherwise; 2: usage or nothing considered."""
import argparse
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True

H8 = r'[0-9a-f]{8}'
H16 = r'[0-9a-f]{16}'
DEC = r'(?:0|[1-9][0-9]{0,9})'
SDEC = r'-?(?:0|[1-9][0-9]{0,9})'
U32 = 4294967295
IO = rf'(?:ok|error:(?:open|write|fsync|close):{H8}|error:write:zero|error:format:(?:record|path))'
CALLS = r'(?:attach|flush|dec|fini|event|go)'
CALL_ID = {'attach': 1, 'flush': 2, 'dec': 3, 'fini': 4, 'event': 5, 'go': 6}
STAGES = {1: 'main-entered', 2: 'prof-init-done', 3: 'thread-begin-done', 4: 'spu-setup-done', 5: 'group-start-B',
          6: 'group-start-D', 7: 'epoch1-report-D', 8: 'drain1-B', 9: 'drain1-D', 10: 'go-B', 11: 'go-D',
          12: 'final-report-D', 13: 'terminate-D', 14: 'join-B', 15: 'join-D', 16: 'lane-end-D', 17: 'spu-release-D',
          18: 'gcm-init-D', 19: 'gpu-attach-D', 20: 'frames-B', 21: 'frames-D', 22: 'detach-D', 23: 'idle-B',
          24: 'idle-D', 25: 'release-D', 26: 'shutdown-B', 27: 'shutdown-D', 28: 'capture-check-D', 99: 'logging-failed'}
REPORT = rf'ok|timeout|identity|spu-status:{CALLS}:{SDEC}'
FAIL = rf'fail=(?:none|{CALLS}:{SDEC})'
# seq: (results, fields).  Fields are checked again against the result in check_record.
MATRIX = {
    1: (r'ok', r'abi=(?:ilp32|lp64)'),
    2: (rf'ok|error:(?:sink-path|init-ex):{H8}', r''),
    3: (rf'ok|error:thread-begin:{H8}', r''),
    4: (rf'ok|error:(?:register-elf|lane-create|spu-init|import|group-create|ea32|thread-init):{H8}',
        rf'(?:lane={DEC})?'),
    5: (r'ok', r''),
    6: (rf'ok|error:group-start:{H8}', r''),
    7: (REPORT, rf'(?:spu_ev1={DEC} {FAIL})?'),
    8: (r'ok', r''),
    9: (rf'ok|timeout|error:(?:drain|stats):{H8}', rf'(?:read_idx={H16} accepted={DEC})?'),
    10: (r'ok', r''),
    11: (rf'ok|mbox-error:{H8}', r''),
    12: (REPORT, rf'(?:spu_ev2={DEC} dec=(?:not-attempted|ok:{SDEC}|failed:{SDEC}) seen_ridx=(?:{H16}|unavailable) '
                 rf'{FAIL})?'),
    13: (rf'ok|not-needed|error:terminate:{H8}', r''),
    14: (r'ok|not-attempted:(?:not-started|terminate-failed)', r''),
    15: (rf'ok|error:join:{H8}', rf'(?:cause={H8} status={H8})?'),
    16: (rf'ok|error:lane-end:{H8}|not-attempted:(?:not-joined|ext-fault)',
         rf'(?:events={DEC} lost={DEC} final=(?:{DEC}|none))?'),
    17: (rf'ok|error:(?:destroy|image-close):{H8}|not-attempted:(?:not-joined|not-created)', r''),
    18: (rf'ok|unavailable:(?:host-buffer|gcm-init):{H8}', r''),
    19: (rf'ok|error:gpu-attach:{H8}', rf'(?:gpu={DEC})?'),
    20: (r'ok', r''),
    21: (rf'ok|timeout|stopped|error:(?:begin|end|mark|frame|drain|stats):{H8}', rf'frames={DEC} events_retired={DEC}'),
    22: (rf'ok|timeout|error:detach:{H8}', r''),
    23: (r'ok', r''),
    24: (r'ok', r''),
    25: (rf'ok|error:release:{H8}', r'released=[01]'),
    26: (r'ok', r''),
    27: (rf'ok|error:shutdown:{H8}', r''),
    28: (rf'ok|error:(?:open|read|size):{H8}', rf'(?:size={DEC} fnv={H16})?'),
    99: (r'failed', r''),
}
FRAMES, EV1, GPU_EV = 30, 132, 90
FIRST_FLUSH = 16  # the SPU core flushes once >= 16 events are pending
# the results each failing SPU call can return (e1_report.h e1_rc_allowed; signed decimal as the markers print them)
_W = {-2, -5, -15, -17, -20, -21, -22}
RC_ALLOWED = {'attach': {-5, -2, -15, -21}, 'flush': _W, 'fini': _W, 'dec': _W - {-22}, 'event': {-21}, 'go': {-1}}


def succ(s, res, f):
    """the records the probe can write right after record s with result res (normal markers only; None = the end)"""
    ok = res == 'ok'
    if s == 1:
        return {2}
    if s == 2:
        return {3} if ok else {None}
    if s == 3:
        return {4} if ok else {26}
    if s == 4:
        if ok:
            return {5, 26}  # 26 only after a logging failure, which this run's present records then show
        return {14} if 'lane' in f else {26}
    if s == 5:
        return {6}
    if s == 6:
        return {7} if ok else {13}
    if s == 7:
        return {8} if ok else {13}
    if s == 8:
        return {9}
    if s == 9:
        return {10, 13} if ok else {13}
    if s == 10:
        return {11}
    if s == 11:
        return {12} if ok else {13}
    if s == 12:
        return {13}
    if s == 13:
        return {14}
    if s == 14:
        return {15} if ok else {16}
    if s == 15:
        return {16}
    if s == 16:
        return {17}
    if s == 17:
        return {18, 26}
    if s == 18:
        return {19, 26} if ok else {26}
    if s == 19:
        return {20} if ok else {26}
    if s == 20:
        return {21}
    if s == 21:
        return {22, 26}
    if s == 22:
        return {23, 26} if ok else {26}
    if s in (23, 24):
        return {s + 1}
    if s == 25:
        return {26}
    if s == 26:
        return {27}
    if s == 27:
        return {28} if ok else {None}
    return {None}


REC = re.compile(rb'EXTP1 run=([0-9a-f]{16}) build=([0-9a-f]{16}) seq=([0-9]{2}) stage=([a-z0-9]+(?:-[a-z0-9]+)*(?:-[BD])?) '
                 rb'result=(\S+) tb=([0-9a-f]{16}) prev=(none|([0-9]{2}):(' + IO.encode() + rb'):([0-9a-f]{16})) '
                 rb'unres=([nuq]),([nu]) counts=(' + DEC.encode() + rb'),(' + DEC.encode() + rb')((?: \S+)*)')
NAME = re.compile(r'extp-([0-9a-f]{16})-([0-9]{2})\.mk')


class Bad(Exception):
    pass


def fields(s):
    return dict(x.split('=', 1) for x in s.split())


def check_record(data, run, seq_s, build):
    if len(data) > 400:
        raise Bad('record longer than 400 bytes')
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
    (r_run, b, seq, stage, result, tb, prev, pseq, pio, pdone, us, ur, cs, cr, fl) = \
        (g.decode() if g is not None else None for g in m.groups())
    s = int(seq)
    if r_run != run or seq != seq_s:
        raise Bad('run/seq differ from the file name')
    if b != build:
        raise Bad(f'build {b} != {build}')
    if STAGES.get(s) != stage:
        raise Bad(f'unknown milestone {seq} {stage}')
    rr, fr = MATRIX[s]
    if not re.fullmatch(rr, result):
        raise Bad(f'stage {seq} cannot carry result {result}')
    fl = fl.strip()
    if not re.fullmatch(fr, fl):
        raise Bad(f'stage {seq} fields {fl!r} do not match the matrix')
    f = fields(fl) if fl else {}
    for k, v in f.items():
        if re.fullmatch(DEC, v) and int(v) > U32 and k not in ('size', 'accepted', 'events', 'lost'):
            raise Bad(f'{k}={v} beyond u32')
    counts = (int(cs), int(cr))
    if counts[0] > 2 * EV1 or counts[1] > GPU_EV:
        raise Bad(f'counts {counts} beyond the build bounds {2 * EV1},{GPU_EV}')
    # the predecessor field
    if s == 1:
        if prev != 'none':
            raise Bad('01 must have prev=none')
    elif prev == 'none':
        raise Bad('only 01 has prev=none')
    elif s == 99:
        if pio == 'ok':
            raise Bad('99 must name a failed predecessor')
        if int(pseq) not in STAGES or int(pseq) == 99:
            raise Bad(f'99 names {pseq}, not a milestone')
    elif pio != 'ok':
        raise Bad('a normal record needs a successful predecessor')
    elif not any(s in succ(int(pseq), r, {'lane': ''}) | succ(int(pseq), r, {}) for r in
                 ('ok', 'error:x', 'timeout', 'identity', 'not-attempted:x', 'unavailable:x', 'mbox-error:x')):
        raise Bad(f'{seq} cannot follow {pseq}')
    # fields vs result
    ok = result == 'ok'
    trusted = ok or result.startswith('spu-status:')
    if s == 4 and result.startswith(('error:register-elf', 'error:lane-create')) and 'lane' in f:
        raise Bad('no lane exists after a register/lane-create failure')
    if s == 4 and not result.startswith(('error:register-elf', 'error:lane-create')) and 'lane' not in f:
        raise Bad('the lane field names a created lane')
    if s in (7, 12) and bool(f) != trusted:
        raise Bad('report fields iff the report is trusted (ok or spu-status)')
    if s in (7, 12) and trusted:
        call, rc = ('none', None) if ok else result.split(':')[1:3]
        if f['fail'] != ('none' if ok else f'{call}:{rc}'):
            raise Bad('fail= must restate the result')
        if not ok and int(rc) not in RC_ALLOWED[call]:
            raise Bad(f'rc {rc} is not a result {call} can return')
    # the PRODUCER-POSSIBLE states of e1_report.h (e1_report_check), as the markers print them
    if s == 7 and trusted:
        call, ev1 = (None if ok else result.split(':')[1]), int(f['spu_ev1'])
        if call in ('dec', 'fini', 'go'):
            raise Bad('report 1 cannot carry a post-GO call')
        if call is None and ev1 != EV1:
            raise Bad('a clean report 1 covers all of epoch 1')
        if call == 'attach' and ev1 != 0:
            raise Bad('an attach failure precedes every event')
        if call in ('flush', 'event') and not FIRST_FLUSH <= ev1 <= EV1:
            raise Bad('no flush (and no flush or event error) before 16 events')
        if counts[0] != ev1:
            raise Bad('07 SPU count != spu_ev1')
    if s == 12 and trusted:
        call = None if ok else result.split(':')[1]
        dec, seen, ev2 = f['dec'], f['seen_ridx'], int(f['spu_ev2'])
        dec_ok, dec_na, seen_ok = dec == 'ok:0', dec == 'not-attempted', seen != 'unavailable'
        if call == 'attach':
            raise Bad('attach failures are reported in report 1')
        if dec.startswith('ok:') and not dec_ok:
            raise Bad('dec ok carries rc 0')
        if call == 'go':
            good = ev2 == 0 and dec_na and not seen_ok
        elif call == 'flush':  # the first post-GO flush, the flush after a dec EAGAIN, or an epoch-2 flush
            good = (ev2 == 0 and dec_na) or (FIRST_FLUSH <= ev2 <= EV1 and dec_ok and seen_ok)
        elif call == 'dec':
            good = ev2 == 0 and dec == f'failed:{result.split(":")[2]}' and seen_ok
        elif call == 'event':
            good = FIRST_FLUSH <= ev2 <= EV1 and dec_ok and seen_ok
        else:  # fini, or no error
            good = ev2 == EV1 and dec_ok and seen_ok
        if not good:
            raise Bad(f'12: not a state the producer can send for {call or "a clean report"} '
                      f'(spu_ev2={ev2} dec={dec} seen_ridx={seen})')
        if counts[0] != EV1 + ev2:
            raise Bad('12 SPU count != 132 + spu_ev2')
    if s == 9 and bool(f) != ok:
        raise Bad('09 fields iff ok')
    if s == 9 and ok and int(f['accepted']) < EV1:
        raise Bad('09 ok needs accepted >= 132')
    if s in (15, 16, 19, 28) and bool(f) != ok:
        raise Bad(f'{seq} fields iff ok')
    if s == 21:
        fr_, ev = int(f['frames']), int(f['events_retired'])
        if fr_ > FRAMES or ev > GPU_EV or ev < 3 * fr_:
            raise Bad('frames/events out of range')
        if ok != (fr_ == FRAMES):
            raise Bad('frames ok iff all 30 frames resolved')
        if ok and ev != GPU_EV:
            raise Bad('30 frames retire 90 events')
        if counts[1] != ev:
            raise Bad('21 GPU count != events_retired')
    if s == 25 and ok and f['released'] != '1':
        raise Bad('a successful release returned the resources')
    # unres vs this record's own result
    if s < 6 and (us, ur) != ('n', 'n'):
        raise Bad('unres before any SPU submission')
    if s < 21 and ur != 'n':
        raise Bad('RSX unres before the frames')
    want = None
    if (s == 6 and not ok) or (s in (7, 12) and result in ('timeout', 'identity')):
        want = 'u'
    elif s == 11 and not ok:
        want = 'q'
    if want and us != want:
        raise Bad(f'{result} at {seq} needs SPU unres {want}, not {us}')
    if s == 21 and result == 'timeout' and ur != 'u':
        raise Bad('a frames timeout needs RSX unres u')
    if s <= 12 and not want and us != 'n' and s != 99:
        raise Bad(f'{result} at {seq}: no SPU submission was stopped before, unres must be n')
    return {'seq': s, 'stage': stage, 'result': result, 'tb': int(tb, 16), 'pseq': int(pseq) if pseq else None,
            'pio': pio, 'pdone': int(pdone, 16) if pdone else None, 'unres': (us, ur), 'counts': counts, 'f': f}


def flow(recs):
    """the probe's flow over the present records: successor and cleanup-state contradictions"""
    bad = []
    by = {r['seq']: r for r in recs}
    norm = [r for r in recs if r['seq'] != 99]
    for a, b in zip(norm, norm[1:]):
        if b['pseq'] == a['seq'] and b['seq'] not in succ(a['seq'], a['result'], a['f']):
            bad.append(f'{b["seq"]:02d} cannot follow {a["seq"]:02d} result {a["result"]}')

    def res(s):
        return by[s]['result'] if s in by else None
    # did a trusted report say the program returned?  True / False only when the PRESENT records decide it; an absent
    # (or invalid) record is unknown, so the rule is not applied then
    if (res(7) or '').startswith('spu-status:') or (res(12) or '').startswith(('ok', 'spu-status:')):
        exited = True
    elif res(7) in ('timeout', 'identity') or res(12) in ('timeout', 'identity') or (res(6) or '').startswith('error:') \
            or (res(9) not in (None, 'ok')) or (res(11) not in (None, 'ok')):
        exited = False
    else:
        exited = None
    if 13 in by:
        if exited is not None and (res(13) == 'not-needed') != exited:
            bad.append('13: terminate is not-needed exactly when a trusted report says the program returned')
        if 6 not in by and 5 not in by and 4 in by and res(4) != 'ok':
            bad.append('13 after a setup failure: the group never started')
    if 14 in by:
        if res(14) == 'not-attempted:not-started' and (4 in by and res(4) == 'ok' or 6 in by or 13 in by):
            bad.append('14 not-started although the group start was issued')
        if res(14) == 'not-attempted:terminate-failed' and 13 in by and not res(13).startswith('error:'):
            bad.append('14 terminate-failed without a failed terminate')
        if res(14) == 'ok' and 13 in by and res(13).startswith('error:'):
            bad.append('14 joins after a failed terminate')
    if 16 in by:
        joined = res(15) == 'ok'
        if res(16) in ('ok',) or res(16).startswith('error:'):
            if 15 in by and not joined or 14 in by and res(14) != 'ok':
                bad.append('16: lane_end only after a successful join')
            if res(9) and res(9).startswith('error:'):
                bad.append('16: lane_end after an extension fault')
        if res(16) == 'not-attempted:not-joined' and joined:
            bad.append('16 not-joined after a successful join')
    if 17 in by:
        if res(17) == 'not-attempted:not-joined' and (res(15) == 'ok' or res(14) == 'not-attempted:not-started'):
            bad.append('17 not-joined although no program can run')
        if res(17) in ('ok',) or res(17).startswith('error:'):
            if res(15) is not None and res(15) != 'ok' or res(14) == 'not-attempted:terminate-failed':
                bad.append('17: release only when no program can run')
    if any(s in by for s in range(18, 26)):
        if res(12) not in (None, 'ok') or res(16) not in (None, 'ok') or res(15) not in (None, 'ok'):
            bad.append('the GPU phase only after a clean SPU phase (report 2 ok, join ok, lane_end ok)')
    if 23 in by and (res(22) not in (None, 'ok') or res(21) not in (None, 'ok')):
        bad.append('idle only after a successful detach of complete frames')
    if 25 in by and 24 not in by and 23 in by:
        bad.append('release before the idle completed')
    if 28 in by and res(27) not in (None, 'ok'):
        bad.append('capture check after a failed shutdown')
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir', type=Path)
    ap.add_argument('--build', required=True)
    ap.add_argument('--run')
    a = ap.parse_args()
    if not re.fullmatch(r'[0-9a-f]{16}', a.build) or (a.run and not re.fullmatch(r'[0-9a-f]{16}', a.run)) or \
            not a.dir.is_dir():
        print('usage: --build/--run 16 lowercase hex, DIR a directory')
        return 2
    runs, present, bad, n = {}, {}, 0, 0
    for p in sorted(a.dir.iterdir()):
        m = NAME.fullmatch(p.name)
        if not m or not p.is_file():
            if p.name.endswith('.mk'):
                print(f'INVALID {p.name}: name does not match')
                bad += 1
            continue
        if a.run and m.group(1) != a.run:
            continue
        n += 1
        present.setdefault(m.group(1), set()).add(int(m.group(2)))
        try:
            r = check_record(p.read_bytes(), m.group(1), m.group(2), a.build)
        except Bad as x:
            bad += 1
            print(f'INVALID {p.name}: {x}')
            continue
        runs.setdefault(m.group(1), []).append(r)
        print(f'VALID {p.name}: {r["stage"]} result={r["result"]}')
    if len(runs) > 1:
        print(f'INCONSISTENT: {len(runs)} runs in one run directory')
        bad += 1
    for run, recs in sorted(runs.items()):
        recs.sort(key=lambda r: r['seq'])
        by = {r['seq']: r for r in recs}
        problems = []
        for r in recs:
            if r['pseq'] is None:
                continue
            between = sorted(s for s in present[run] if r['pseq'] < s < r['seq'] and s != 99)
            if between and r['seq'] != 99:
                problems.append(f'{r["seq"]:02d} names {r["pseq"]:02d} but present marker(s) {between} lie between')
            p = by.get(r['pseq'])
            if p and not (p['tb'] <= r['pdone'] <= r['tb']):
                problems.append(f'{r["seq"]:02d}: time order broken against {r["pseq"]:02d}')
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
        problems += flow(recs)
        facts = ['REACHED ' + ' '.join(f'{r["seq"]:02d}' for r in recs)]
        if not problems:
            if any(r['unres'][0] == 'u' for r in recs):
                facts.append('UNRESOLVED_SUBMISSION spu')
            if any(r['unres'][1] == 'u' for r in recs):
                facts.append('UNRESOLVED_SUBMISSION rsx')
            if any(r['unres'][0] == 'q' for r in recs):
                facts.append('SUBMISSION_OUTCOME_UNKNOWN spu')
            if 15 in by and by[15]['result'] == 'ok':
                facts.append('SPU_JOINED')
            if 9 in by and by[9]['result'] == 'ok':
                facts.append(f'READ_IDX {by[9]["f"]["read_idx"]}')
            if 12 in by and 'seen_ridx' in by[12]['f']:
                facts.append(f'SEEN_RIDX {by[12]["f"]["seen_ridx"]}')
            if 21 in by:
                facts.append(f'GPU_FRAMES {by[21]["f"]["frames"]} events {by[21]["f"]["events_retired"]}')
            if 99 in by:
                facts.append(f'LOGGING_FAILURE {by[99]["pseq"]:02d} {by[99]["pio"]}')
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
