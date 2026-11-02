#!/usr/bin/env python3
"""R1 fail-closed oracle: each <name>.prof against <name>.truth (what the producer and the emulated GPU did) and
<name>.expect (the trace's own claims).  Independent of prg.c: it decodes the capture bytes itself.

Fail-closed inputs: the directory must hold EXACTLY the pinned inventory of its suite (each name with .prof, .truth
and .expect); every .expect directive is known, scalars appear once, the required fields are present; every .truth
row has 7 integers, numbers are unique and contiguous from 0; every frame's CRC and file sequence are checked.

Checks, per GPU domain, in ACTUAL FILE ORDER (never sorted): GPU_EVENTS and LOSS tile [0, next_seq) exactly; every
event's (desc, kind, depth) equals the truth for its number, that number was submitted AND executed by the GPU, and its
raw value equals the GPU's true timer (a mismatch is the EXPECTED outcome only for traces marked `undetected 1`, the
A1/A2 counterexamples the runtime cannot see); every LOSS number was numbered; a reason-3 number was lost at
submission, except inside the domain's LAST LOSS of a trace marked `exhausted 1` (the terminal LOSS also covers events
the exhausted LOSS budget withheld); nothing of the domain follows its FINAL; FINAL is present exactly when
`final_written 1`, with that state, next_seq = the tiling end and counts = the delivered events and losses; the
`events`, `lost`, `loss` (the exact LOSS list, in order), `loss-chunks` and `brackets` claims hold.  Narrow
exceptions: `sink_dead 1` (a failed sink: the claims are only the exit code, the tiling may stop early) and
`noreader` (a rebased sequence, checked in-trace).

usage: rsx_oracle.py DIR --suite traces|threads   (exit 0 all consistent, 1 otherwise)"""
import argparse
import os
import struct
import sys
import zlib

INVENTORY = {
    'traces': '''r1-attach-bounds r1-cal-bad-second-label r1-cal-clock-fail r1-cal-epoch-exact r1-cal-epoch-over
        r1-cal-file-exact r1-cal-file-over r1-cal-frontier-frozen r1-cal-poison r1-cal-seal-during r1-cal-tb-order
        r1-calibrate r1-cex-late-report-benign r1-cex-stale-monotone r1-cex-torn-read r1-ctl-decrease
        r1-ctl-label-backward r1-ctl-label-before-report r1-ctl-label-future r1-deferral r1-detach-open-loss
        r1-detach-retry r1-detach-then-shutdown r1-detach-vs-seal r1-end-lost r1-fault-cal-bracket-sunlock
        r1-fault-cal-slock r1-fault-detach-elock r1-fault-detach-eunlock r1-fault-detach-final-slock
        r1-fault-detach-final-sunlock r1-fault-detach-hook-eunlock r1-fault-detach-poll-slock
        r1-fault-detach-poll-sunlock r1-fault-drain-elock r1-fault-drain-eunlock r1-fault-drain-slock
        r1-fault-drain-sunlock r1-fault-producer-slock r1-fault-producer-sunlock r1-fault-seal-elock
        r1-fault-seal-eunlock r1-fault-seal-slock r1-fault-seal-sunlock r1-fifo-refusal r1-first-use-stale
        r1-full-ring r1-intervals r1-l2-reject-flush r1-label-beyond-issued r1-label-wrap r1-loss-budget-exact
        r1-loss-budget-over r1-mark-depth64 r1-nested-suppression r1-overflow r1-pair-desc r1-pair-suppressed
        r1-seal-calibrating r1-seal-inflight r1-seal-pending r1-second-label-advance r1-sink-fail-events
        r1-sink-fail-final r1-sink-fail-trailing-loss r1-small-ring-refill r1-steady r1-exhaust-admitted
        r1-exhaust-at-seal r1-l2-back-beyond-issued r1-l2-back-beyond-issued-wrap r1-pair-suppressed-stale'''.split(),
    'threads': ['r1-thr-run', 'r1-thr-seal-live', 'r1-thr-detach'],
}
DOMAIN = 7
KNOWN_CHUNKS = {1, 2, 3, 4, 6, 8, 9, 10, 14, 15, 16}
BASE_REQUIRED = {'final_state', 'retired', 'final_written', 'sink_dead'}
SCALARS = BASE_REQUIRED | {'exit', 'events', 'lost', 'brackets', 'undetected', 'undetected-benign', 'loss-chunks',
                           'exhausted', 'noreader'}
MULTI = {'loss'}
BOOLEAN = {'retired', 'final_written', 'sink_dead', 'undetected', 'undetected-benign', 'exhausted', 'noreader'}
NOREADER = {'r1-overflow'}  # the only trace allowed the rebased-sequence exemption


class Bad(Exception):
    pass


def parse_expect(path, stem):
    exp = {}
    for n, l in enumerate(open(path, encoding='ascii'), 1):
        l = l.rstrip('\n')
        if not l.strip():
            raise Bad(f'{path}:{n}: empty line')
        k, _, v = l.partition(' ')
        if k not in SCALARS and k not in MULTI:
            raise Bad(f'unknown directive {k!r}')
        if k == 'noreader':
            v = '1' if v == '' else v
        try:
            vals = tuple(int(x) for x in v.split())
        except ValueError:
            raise Bad(f'directive {k!r}: non-integer value {v!r}')
        if k in MULTI:
            if len(vals) != 3:
                raise Bad(f'directive {k!r}: want 3 values, got {v!r}')
            exp.setdefault(k, []).append(vals)
        else:
            if k in exp:
                raise Bad(f'duplicate directive {k!r}')
            if len(vals) != 1:
                raise Bad(f'directive {k!r}: want 1 value, got {v!r}')
            if k in BOOLEAN and vals[0] not in (0, 1):
                raise Bad(f'directive {k!r}: boolean, got {vals[0]}')
            exp[k] = vals[0]
    if exp.get('noreader') and stem not in NOREADER:
        raise Bad(f'noreader exemption not permitted for {stem}')
    if 'noreader' in exp and not exp['noreader']:
        raise Bad('noreader 0: omit the directive instead')
    need = set(BASE_REQUIRED)
    if 'noreader' not in exp:
        need.add('exit')
        if not exp.get('sink_dead'):
            need |= {'events', 'lost'}
    missing = need - exp.keys()
    if missing:
        raise Bad(f'missing required directive(s) {sorted(missing)}')
    return exp


def parse_truth(path, rebased):
    truth, prev = {}, None
    for n, l in enumerate(open(path, encoding='ascii'), 1):
        f = l.split()
        if len(f) != 7:
            raise Bad(f'{os.path.basename(path)}:{n}: {len(f)} fields, want 7')
        try:
            s, desc, kind, depth, sub, ran, tt = map(int, f)
        except ValueError:
            raise Bad(f'{os.path.basename(path)}:{n}: non-integer field')
        if s in truth:
            raise Bad(f'duplicate truth row for number {s}')
        if sub not in (0, 1) or ran not in (0, 1) or (ran and not sub):
            raise Bad(f'truth row {s}: submitted {sub} executed {ran}')
        if (prev is None and s != 0 and not rebased) or (prev is not None and s != prev + 1):
            raise Bad(f'truth rows not contiguous at {s} (previous {prev})')
        truth[s] = (desc, kind, depth, sub, ran, tt)
        prev = s
    return truth


def frames(b):
    if len(b) < 16 or b[:8] != b'PS3DKPRF':
        raise Bad('bad magic or header')
    o, want_seq = 16, 0
    while o < len(b):
        if o + 16 > len(b):
            raise Bad(f'truncated frame header at {o}')
        t, ln, seq = struct.unpack_from('<IIQ', b, o)
        end = o + 16 + ln
        if end + 4 > len(b):
            raise Bad(f'frame at {o} runs past the file')
        if struct.unpack_from('<I', b, end)[0] != zlib.crc32(b[o:end]):
            raise Bad(f'frame at {o}: CRC mismatch')
        if seq != want_seq:
            raise Bad(f'frame at {o}: file seq {seq}, want {want_seq}')
        want_seq += 1
        yield t, b[o + 16:end]
        o = end + 4


def check(base, exp):
    rebased = exp.get('noreader') == 1
    truth = parse_truth(base + '.truth', rebased)
    seq_chunks, final, finals, brackets, after_final = [], None, 0, 0, False
    for t, p in frames(open(base + '.prof', 'rb').read()):
        if t not in KNOWN_CHUNKS:
            raise Bad(f'unknown chunk type {t}')
        if t in (10, 14, 15, 16) and len(p) < 4:
            raise Bad(f'chunk {t}: short payload')
        if t == 14:
            dom, ep, first, n = struct.unpack_from('<IIQI', p, 0)
            if dom != DOMAIN or len(p) != 20 + 28 * n or n == 0:
                raise Bad(f'GPU_EVENTS: domain {dom} n {n} len {len(p)}')
            evs = [(first + i,) + struct.unpack_from('<QIBB', p, 20 + 28 * i) for i in range(n)]
            seq_chunks.append(('E', first, n, evs))
            after_final |= finals > 0
        elif t == 15:
            if len(p) != 28:
                raise Bad(f'LOSS length {len(p)}')
            src, sk, reason, _, first, count, _ = struct.unpack_from('<IBBHQQI', p, 0)
            if src != DOMAIN or sk != 2:
                raise Bad(f'LOSS source {src}/{sk}')
            seq_chunks.append(('L', first, count, reason))
            after_final |= finals > 0
        elif t == 16:
            cnt, dom = struct.unpack_from('<II', p, 0)
            if cnt != 1 or dom != DOMAIN:
                raise Bad(f'DOMAIN_FINAL count {cnt} domain {dom}')
            final = struct.unpack_from('<IBBHQQQI', p, 4)
            finals += 1
        elif t == 10:
            dom, _, _, n = struct.unpack_from('<IIQI', p, 0)
            if dom != DOMAIN:
                raise Bad(f'CALIBRATION domain {dom}')
            brackets += n
            after_final |= finals > 0
    bad = []
    if after_final:
        bad.append('domain chunks after its FINAL')
    if finals > 1:
        bad.append(f'{finals} FINAL chunks')
    if rebased:
        return bad, 'rebased (checked in-trace)'
    if bool(finals) != bool(exp['final_written']):
        bad.append(f'FINAL present {bool(finals)}, claimed final_written {exp["final_written"]}')
    # tiling in file order
    at, events, losses = 0, [], []
    for c in seq_chunks:
        if c[1] != at:
            bad.append(f'tiling: {"GPU_EVENTS" if c[0] == "E" else "LOSS"} at {c[1]}, expected {at} (file order)')
            break
        at = c[1] + c[2]
        if c[0] == 'E':
            events += c[3]
        else:
            losses.append(c[1:])
    if final is not None:
        if final[1] != 2 or final[2] != exp['final_state']:  # (domain, kind, state, pad, next_seq, events, lost, open)
            bad.append(f'FINAL kind {final[1]} state {final[2]} != GPU / claimed {exp["final_state"]}')
        if at != final[4]:
            bad.append(f'tiling ends at {at}, FINAL next_seq {final[4]}')
    mismatches = 0
    for s, raw, desc, kind, depth in events:
        tr = truth.get(s)
        if tr is None or not tr[3]:
            bad.append(f'event {s} was never submitted')
            continue
        if not tr[4]:
            bad.append(f'event {s} was emitted but never executed by the GPU')
            continue
        if (desc, kind, depth) != tr[:3]:
            bad.append(f'event {s}: identity {(desc, kind, depth)} != truth {tr[:3]}')
        if raw != tr[5]:
            mismatches += 1
    for i, (first, count, reason) in enumerate(losses):
        terminal_exhausted = exp.get('exhausted') == 1 and i == len(losses) - 1
        for s in range(first, first + count):
            tr = truth.get(s)
            if tr is None:
                bad.append(f'LOSS covers {s}, which was never numbered')
                break
            if reason == 3 and tr[3] and not terminal_exhausted:
                bad.append(f'LOSS reason 3 covers {s}, which was submitted')
                break
    nev, nlost = len(events), sum(c for _, c, _ in losses)
    if final is not None and (final[5] != nev or final[6] != nlost):
        bad.append(f'FINAL events/lost {final[5]}/{final[6]} != delivered {nev}/{nlost}')
    if exp.get('sink_dead'):
        return bad, 'sink failed (exit code only)'
    if exp['events'] != nev:
        bad.append(f'events {nev} != claimed {exp["events"]}')
    if exp['lost'] != nlost:
        bad.append(f'lost {nlost} != claimed {exp["lost"]}')
    if 'loss' in exp and exp['loss'] != losses:
        bad.append(f'LOSS list {losses} != claimed {exp["loss"]}')
    if 'loss-chunks' in exp and exp['loss-chunks'] != len(losses):
        bad.append(f'{len(losses)} LOSS chunks != claimed {exp["loss-chunks"]}')
    if 'brackets' in exp and exp['brackets'] != brackets:
        bad.append(f'brackets {brackets} != claimed {exp["brackets"]}')
    if exp.get('undetected-benign') == 1:  # A1 violated invisibly, but the report landed before the read
        if mismatches:
            bad.append(f'a benign counterexample with {mismatches} wrong values')
        return bad, 'UNDETECTED-BENIGN (A1 violated, value correct)'
    undetected = exp.get('undetected') == 1
    if undetected and not mismatches:
        bad.append('a counterexample with NO value mismatch: it did not exercise what it claims')
    if not undetected and mismatches:
        bad.append(f'{mismatches} event values differ from the GPU truth')
    return bad, f'VALUE-MISMATCH {mismatches} (expected: undetected by the runtime)' if undetected else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--suite', required=True, choices=sorted(INVENTORY))
    a = ap.parse_args()
    want = set(INVENTORY[a.suite])
    have = {}
    for f in os.listdir(a.dir):
        stem, ext = os.path.splitext(f)
        if ext in ('.prof', '.truth', '.expect'):
            have.setdefault(stem, set()).add(ext)
    fails, ok = 0, 0
    for stem in sorted(set(have) | want):
        if stem not in want:
            print(f'{stem}: ORACLE FAIL not in the pinned {a.suite} inventory')
            fails += 1
            continue
        missing = {'.prof', '.truth', '.expect'} - have.get(stem, set())
        if missing:
            print(f'{stem}: ORACLE FAIL missing {" ".join(sorted(missing))} (inventory)')
            fails += 1
            continue
        base = os.path.join(a.dir, stem)
        try:
            bad, note = check(base, parse_expect(base + '.expect', stem))
        except Bad as x:
            bad, note = [str(x)], ''
        except Exception as x:  # fail closed
            bad, note = [f'oracle error {x!r}'], ''
        if bad:
            fails += 1
            print(f'{stem}: ORACLE FAIL ' + '; '.join(bad))
        else:
            ok += 1
            print(f'{stem}: oracle PASS {note}'.rstrip())
    print(f'oracle: {ok} pass, {fails} fail ({len(want)} pinned)')
    return 1 if fails or ok != len(want) else 0


if __name__ == '__main__':
    sys.exit(main())
