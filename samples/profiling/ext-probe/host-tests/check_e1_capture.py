#!/usr/bin/env python3
"""E1 TRANSPORT ACCEPTANCE (design p3.3 E1 r2 section 5 + r3 section E; review 10:25): one run directory's markers and
its capture, read HERE by the pinned reader (tools/ps3dk-prof example ext_facts).  Fail-closed: anything not stated
below as accepted is a REJECT.

usage: check_e1_capture.py RUNDIR --build BUILD --facts-bin EXT_FACTS --facts-bin-sha256 HEX [--capture PROF]
  (PROF defaults to the single extp-*.prof in RUNDIR)

The reader runs on the EXACT bytes this checker hashed (a private copy); it must exit 0 with an EMPTY stderr and a
complete output (header ... end).  No facts are taken from anywhere else.

ACCEPT requires all of:
  markers: validate_e1_markers.py exits 0 on RUNDIR; records 01..28 all present, every result ok except 13 =
  not-needed (report 2 trusted: the program returned), no 99; 09 read_idx == 12 seen_ridx (full 64 bits);
  07 spu_ev1 132, 12 spu_ev2 132 dec ok:0 fail=none; 16 events=264 lost=0 final=1; 21 frames=30 events_retired=90;
  28 size and fnv == the capture's byte length and FNV-1a 64;
  facts, a CLOSED inventory (every row must be expected, each key exactly once):
    file: malformed=0 verdict=complete incomplete=0 ext=1 (exactly one line);
    sources: EXACTLY {lane <04 lane>, gpu <19 gpu>}, once each;
    lane: structure complete, final 1 (FINALIZED), events 264, lost 0, losses 0, epochs 2, paired 128, marks 8,
      unpaired 0, censored 0, frames 0/0/0; events rows EXACTLY {epoch 1: begin/end probe-e1 64, mark probe-mark 4;
      epoch 2: begin/end probe-e2 64, mark probe-mark 4};
    gpu: structure complete, raw_valid 1, final 1, events 90, lost 0, losses 0, epochs 1, paired 30, marks 30,
      unpaired 0, censored 0, frames resolved 30, unresolved 0, none 0; events rows EXACTLY {epoch 1: begin/end
      e1-gpu 30, mark e1-gpu-mark 30};
    epochrec rows EXACTLY, in this order: lane domain epoch 1 reason 1 written 0; lane domain epoch 2 reason 2
      written 0x00000000ffffffff; gpu domain epoch 1 reason 1 written 0;
    variation rows EXACTLY one per lane epoch (1 and 2) - their STATE is an OBSERVATION, never a criterion, but a
      missing row is a REJECT (the classification must have been made).
NOT criteria (printed as OBSERVATIONS only): the lane's RAW VALIDITY (NOT VALID by the epoch-transition rule is
expected) and each epoch's counter state.  Exit 0 ACCEPT, 1 REJECT, 2 usage/unreadable."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
EV1, GPU_EV, FRAMES = 132, 90, 30
LINE = {
    'file': re.compile(r'file malformed=0 verdict=(complete|incomplete) incomplete=(\d+) ext=([01])'),
    'source': re.compile(r'source kind=(lane|gpu) id=(\d+) domain=(\d+) structure=(complete|incomplete) raw_valid=([01]) '
                         r'final=(\d+|none) events=(\d+) lost=(\d+) losses=(\d+) epochs=(\d+) paired=(\d+) marks=(\d+) '
                         r'unpaired=(\d+) censored=(\d+) frames_resolved=(\d+) frames_unresolved=(\d+) '
                         r'frames_none=(\d+)'),
    'events': re.compile(r'events kind=(lane|gpu) id=(\d+) epoch=(\d+) ev=(begin|end|mark|\?) desc=(\S+) count=(\d+)'),
    'epochrec': re.compile(r'epochrec domain=(\d+) epoch=(\d+) reason=(\d+) written=([0-9a-f]{16})'),
    'variation': re.compile(r'variation id=(\d+) epoch=(\d+) state=(insufficient|static|moving)'),
}
SRC_KEYS = ('kind', 'id', 'domain', 'structure', 'raw_valid', 'final', 'events', 'lost', 'losses', 'epochs', 'paired',
            'marks', 'unpaired', 'censored', 'frames_resolved', 'frames_unresolved', 'frames_none')


def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_facts(text, bad):
    if not text.endswith('\n'):
        bad.append('facts: output not newline-terminated (truncated)')
        return None
    lines = text.split('\n')
    lines.pop()
    if not lines or lines[0] != 'E1FACTS 1':
        bad.append('facts: missing E1FACTS 1 header')
        return None
    if lines[-1] != 'end' or lines.count('end') != 1:
        bad.append('facts: missing or repeated end line (truncated)')
        return None
    f = {'file': [], 'source': [], 'events': [], 'epochrec': [], 'variation': []}
    for ln in lines[1:-1]:
        kind = ln.split(' ', 1)[0]
        rx = LINE.get(kind)
        m = rx.fullmatch(ln) if rx else None
        if not m:
            bad.append(f'facts: unparsed line {ln!r}')
            continue
        f[kind].append(m.groups())
    if len(f['file']) != 1:
        bad.append(f'facts: {len(f["file"])} file lines (MALFORMED or not produced)')
        return None
    return f


def marker_fields(rundir):
    recs = {}
    for p in rundir.glob('extp-*-*.mk'):
        m = re.fullmatch(r'extp-[0-9a-f]{16}-([0-9]{2})\.mk', p.name)
        if not m:
            continue
        t = p.read_text(encoding='ascii', errors='replace').rstrip('\n')
        body = t[:t.rfind(' len=')]
        r = re.search(r' result=(\S+) ', body)
        if not r or ' counts=' not in body:
            recs[int(m.group(1))] = ('UNPARSED', {})
            continue
        tail = body.split(' counts=', 1)[1].split(' ', 1)
        fl = dict(x.split('=', 1) for x in tail[1].split()) if len(tail) > 1 else {}
        recs[int(m.group(1))] = (r.group(1), fl)
    return recs


def exact(bad, what, got, want):
    if sorted(got) != sorted(want) or len(set(got)) != len(got):
        dup = sorted({g for g in got if got.count(g) > 1})
        bad.append(f'facts: {what} rows {sorted(got)} != exactly {sorted(want)}' + (f' (duplicates {dup})' if dup else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rundir', type=Path)
    ap.add_argument('--build', required=True)
    ap.add_argument('--facts-bin', type=Path, required=True)
    ap.add_argument('--facts-bin-sha256', required=True)
    ap.add_argument('--capture', type=Path)
    a = ap.parse_args()
    if not a.rundir.is_dir() or not a.facts_bin.is_file() or not re.fullmatch(r'[0-9a-f]{64}', a.facts_bin_sha256):
        print('usage: RUNDIR a directory, --facts-bin a file, --facts-bin-sha256 64 hex')
        return 2
    caps = sorted(a.rundir.glob('extp-*.prof')) if a.capture is None else [a.capture]
    if len(caps) != 1 or not caps[0].is_file():
        print(f'REJECT: expected exactly one capture, found {len(caps)}')
        return 1
    bad, obs = [], []
    if hashlib.sha256(a.facts_bin.read_bytes()).hexdigest() != a.facts_bin_sha256:
        print('REJECT: the facts reader is not the pinned binary')
        return 1
    cap = caps[0].read_bytes()
    with tempfile.TemporaryDirectory() as td:  # the reader reads exactly the bytes hashed here
        priv = Path(td) / 'capture.prof'
        priv.write_bytes(cap)
        try:
            fr = subprocess.run([str(a.facts_bin), str(priv)], capture_output=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as e:
            print(f'REJECT: the facts reader did not run: {e}')
            return 1
    if fr.returncode != 0:
        bad.append(f'facts: reader exit {fr.returncode}')
    if fr.stderr:
        bad.append(f'facts: reader stderr {fr.stderr[:200]!r}')
    try:
        ftext = fr.stdout.decode('ascii')
    except UnicodeDecodeError:
        bad.append('facts: reader output not ASCII')
        ftext = ''

    # markers
    v = subprocess.run([sys.executable, '-I', '-B', str(HERE / 'validate_e1_markers.py'), str(a.rundir), '--build',
                        a.build], capture_output=True, text=True)
    if v.returncode != 0:
        bad.append(f'markers: validator exit {v.returncode}: ' +
                   '; '.join(l for l in v.stdout.splitlines() if not l.startswith('VALID'))[:600])
    m = marker_fields(a.rundir)
    missing = [s for s in range(1, 29) if s not in m]
    if missing:
        bad.append(f'markers: missing {missing}')
    if 99 in m:
        bad.append('markers: a logging failure (99)')
    want_res = {s: ('not-needed' if s == 13 else 'ok') for s in range(1, 29)}
    notok = [f'{s:02d}={m[s][0]}' for s in sorted(m) if s != 99 and m[s][0] != want_res.get(s)]
    if notok:
        bad.append(f'markers: results not as a clean run writes them: {notok}')

    def mf(s, k):
        return m.get(s, (None, {}))[1].get(k)
    if mf(9, 'read_idx') is None or mf(9, 'read_idx') != mf(12, 'seen_ridx'):
        bad.append(f'read_idx 09 {mf(9, "read_idx")} != seen_ridx 12 {mf(12, "seen_ridx")}')
    if (mf(7, 'spu_ev1'), mf(12, 'spu_ev2'), mf(12, 'dec'), mf(12, 'fail')) != (str(EV1), str(EV1), 'ok:0', 'none'):
        bad.append('markers: the SPU reports do not state 132 + 132 events, dec ok, no error')
    if (mf(16, 'events'), mf(16, 'lost'), mf(16, 'final')) != (str(2 * EV1), '0', '1'):
        bad.append('markers: lane_end does not state 264 events, 0 lost, FINAL 1')
    if (mf(21, 'frames'), mf(21, 'events_retired')) != (str(FRAMES), str(GPU_EV)):
        bad.append('markers: frames do not state 30 frames / 90 events')
    if mf(28, 'size') != str(len(cap)) or mf(28, 'fnv') != f'{fnv(cap):016x}':
        bad.append('markers: 28 size/fnv differ from the capture read here')

    # facts: the closed inventory
    if ftext == '':
        bad.append('facts: no reader output')
    f = parse_facts(ftext, bad) if ftext else None
    if f is not None:
        verdict, n_inc, ext = f['file'][0]
        if (verdict, n_inc, ext) != ('complete', '0', '1'):
            bad.append(f'facts: file verdict={verdict} incomplete={n_inc} ext={ext} (want complete, 0, 1)')
        lane_id, gpu_id = mf(4, 'lane'), mf(19, 'gpu')
        exact(bad, 'source', [(s[0], s[1]) for s in f['source']], [('lane', lane_id), ('gpu', gpu_id)])
        src = {(s[0], s[1]): dict(zip(SRC_KEYS, s)) for s in f['source']}
        lane, gpu = src.get(('lane', lane_id)), src.get(('gpu', gpu_id))
        want_lane = {'structure': 'complete', 'final': '1', 'events': str(2 * EV1), 'lost': '0', 'losses': '0',
                     'epochs': '2', 'paired': '128', 'marks': '8', 'unpaired': '0', 'censored': '0',
                     'frames_resolved': '0', 'frames_unresolved': '0', 'frames_none': '0'}
        want_gpu = {'structure': 'complete', 'raw_valid': '1', 'final': '1', 'events': str(GPU_EV), 'lost': '0',
                    'losses': '0', 'epochs': '1', 'paired': str(FRAMES), 'marks': str(FRAMES), 'unpaired': '0',
                    'censored': '0', 'frames_resolved': str(FRAMES), 'frames_unresolved': '0', 'frames_none': '0'}
        for name, s, want in (('lane', lane, want_lane), ('gpu', gpu, want_gpu)):
            if s is None:
                continue
            for k, w in want.items():
                if s[k] != w:
                    bad.append(f'facts: {name} {k}={s[k]} (want {w})')
        if lane is not None:
            obs.append(f'OBSERVATION lane {lane_id}: raw_valid={lane["raw_valid"]} (NOT VALID expected by the epoch rule)')
        ev_want = [('lane', lane_id, '1', 'begin', 'probe-e1', '64'), ('lane', lane_id, '1', 'end', 'probe-e1', '64'),
                   ('lane', lane_id, '1', 'mark', 'probe-mark', '4'), ('lane', lane_id, '2', 'begin', 'probe-e2', '64'),
                   ('lane', lane_id, '2', 'end', 'probe-e2', '64'), ('lane', lane_id, '2', 'mark', 'probe-mark', '4'),
                   ('gpu', gpu_id, '1', 'begin', 'e1-gpu', '30'), ('gpu', gpu_id, '1', 'end', 'e1-gpu', '30'),
                   ('gpu', gpu_id, '1', 'mark', 'e1-gpu-mark', '30')]
        exact(bad, 'events', list(f['events']), ev_want)
        ld = lane['domain'] if lane else None
        gd = gpu['domain'] if gpu else None
        er_want = [(ld, '1', '1', '0000000000000000'), (ld, '2', '2', '00000000ffffffff'),
                   (gd, '1', '1', '0000000000000000')]
        if list(f['epochrec']) != er_want:
            bad.append(f'facts: epochrec rows {f["epochrec"]} != exactly, in order, {er_want}')
        exact(bad, 'variation', [(x[0], x[1]) for x in f['variation']], [(lane_id, '1'), (lane_id, '2')])
        for x in f['variation']:
            obs.append(f'OBSERVATION lane {x[0]} epoch {x[1]}: counter {x[2].upper()}')
    for o in obs:
        print(o)
    for b in bad:
        print(f'REJECT: {b}')
    print('ACCEPT' if not bad else f'REJECT ({len(bad)} reason(s))')
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
