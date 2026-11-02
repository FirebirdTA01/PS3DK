#!/usr/bin/env python3
"""R2 fail-closed oracle: each <name>.prof against <name>.truth (every NUMBERED GPU call of the case, in number
order: seq kind depth descriptor-name owner frame) and <name>.expect (the case's claims).  Independent of the
runtime: it decodes the capture bytes itself.

Inputs: EXACTLY the pinned inventory (each with .prof .truth .expect); known directives only, scalars once, booleans
0/1; truth rows exactly 6 fields, numbers unique and contiguous from 0.  Frames: CRC and file sequence.

Per GPU domain (ids 1026..1089), in ACTUAL FILE ORDER: GPU_EVENTS and LOSS tile [0, next_seq) with nothing after
its FINAL; FINAL present exactly when `final_present 1`, with that state (`final_state`), next_seq = the tiling end
and counts = the delivered events/losses.  Every event matches the truth for its number: its descriptor index names
a DESCRIPTORS entry declared EARLIER in the file with scope 2 and the truth's name; kind, depth, frame owner and frame
id equal the truth; a nonzero owner was declared in a THREADS chunk earlier in the file (never rewritten: an orphan
stays out of the file).  Every LOSS number is a truth number.  Totals: `events`, `lost`, `loss` (exact list, in
order), `loss-chunks`, `brackets`, `domains` (GPU DOMAIN declarations).  Narrow exceptions, each pinned to named
cases (EXEMPT): `sink_dead 1` (the accepted PREFIX is still checked: frames, tiling, event identity, owners, and any
FINAL that was accepted; only the totals and the FINAL-presence claim are skipped), `preexisting_malformed 1` (the
reader refuses the file for a CPU-side reason; every GPU check still runs), `not_quiesced 1`, `lossy 1`.

usage: gpu_oracle.py DIR --suite plain|tsan   (exit 0 all consistent, 1 otherwise)"""
import argparse
import os
import struct
import sys
import zlib

INVENTORY = {
    'plain': '''r2-combo-gpu r2-combo-both r2-api-refusals r2-lifetime r2-steady r2-two-owner r2-desc-hold r2-orphan
        r2-sink-fail-descriptors r2-sink-fail-events r2-sink-fail-final r2-detach-timeout-seal r2-sched-stale
        r2-sched-after-check r2-sched-in-reserve r2-detach-inline r2-gpu-exhaust r2-fault-attach-lock
        r2-fault-attach-unlock r2-fault-detach-unlock r2-fault-release-unlock r2-fault-seal-unlock r2-fault-snap-lock
        r2-fault-snap-unlock r2-tags r2-lease r2-detach-overlap r2-detach-app-helper r2-snap-first-lock
        r2-snap-first-unlock r2-frame-thread-lock r2-frame-thread-unlock r2-fault-stats-unlock r2-dropped-frame
        r2-mixed-spu r2-mixed-corrupt r2-completion-held r2-completion-fail r2-completion-unlock r2-detach-seal-wins
        r2-frame-unlock-invalid r2-frame-unlock-busy r2-ack-lifetime-1 r2-ack-lifetime-2'''.split(),
    'tsan': ['r2-tsan-run', 'r2-mixed-tsan', 'r2-detach-overlap', 'r2-completion-held', 'r2-completion-unlock',
             'r2-ack-lifetime-1', 'r2-ack-lifetime-2'],
}
# every exemption is pinned to the named cases that need it (any other use is an error)
EXEMPT = {
    'sink_dead': set('''r2-sink-fail-descriptors r2-sink-fail-events r2-sink-fail-final r2-fault-attach-lock
        r2-fault-attach-unlock r2-fault-detach-unlock r2-fault-release-unlock r2-fault-seal-unlock
        r2-frame-thread-lock r2-frame-thread-unlock r2-fault-stats-unlock r2-mixed-corrupt r2-completion-fail
        r2-completion-unlock r2-frame-unlock-invalid r2-frame-unlock-busy'''.split()),
    'preexisting_malformed': {'r2-orphan'},
    'not_quiesced': {'r2-sched-after-check', 'r2-sched-in-reserve'},
    'lossy': {'r2-tsan-run', 'r2-mixed-tsan'},
}
BOOLEAN = {'final_present', 'sink_dead', 'exhausted', 'not_quiesced', 'preexisting_malformed', 'lossy', 'spu_truth'}
SCALARS = BOOLEAN | {'exit', 'events', 'lost', 'final_state', 'brackets', 'domains', 'loss-chunks', 'unresolved_frames'}
MULTI = {'loss'}
KNOWN = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
GPU_IDS = range(1026, 1090)


class Bad(Exception):
    pass


def parse_expect(path, stem):
    exp = {}
    for n, l in enumerate(open(path, encoding='ascii'), 1):
        l = l.rstrip('\n')
        k, _, v = l.partition(' ')
        if k not in SCALARS and k not in MULTI:
            raise Bad(f'unknown directive {k!r}')
        try:
            vals = tuple(int(x) for x in v.split())
        except ValueError:
            raise Bad(f'{k}: non-integer {v!r}')
        if k in MULTI:
            if len(vals) != 3:
                raise Bad(f'{k}: want 3 values')
            exp.setdefault(k, []).append(vals)
            continue
        if k in exp:
            raise Bad(f'duplicate directive {k!r}')
        if len(vals) != 1 or (k in BOOLEAN and vals[0] not in (0, 1)):
            raise Bad(f'{k}: bad value {v!r}')
        exp[k] = vals[0]
    for k, names in EXEMPT.items():
        if exp.get(k) and stem not in names:
            raise Bad(f'exemption {k} not permitted for {stem}')
    need = {'exit'} | (set() if exp.get('sink_dead') else {'events', 'lost', 'final_state', 'final_present'})
    if need - exp.keys():
        raise Bad(f'missing {sorted(need - exp.keys())}')
    return exp


def parse_truth(path):
    tr = []
    for n, l in enumerate(open(path, encoding='ascii'), 1):
        f = l.split()
        if len(f) != 6:
            raise Bad(f'truth line {n}: {len(f)} fields')
        if int(f[0]) != len(tr):
            raise Bad(f'truth line {n}: number {f[0]}, want {len(tr)} (unique, contiguous)')
        tr.append((int(f[1]), int(f[2]), f[3], int(f[4]), int(f[5])))
    return tr


def frames(b, torn_ok):
    if len(b) < 16 and torn_ok and b'PS3DKPRF'.startswith(b[:8]):
        return  # a dead sink before the header was complete (exit 1): nothing to check
    if len(b) < 16 or b[:8] != b'PS3DKPRF':
        raise Bad('bad header')
    o, want = 16, 0
    while o < len(b):
        if o + 16 > len(b):
            return  # a torn tail (sink failures): the reader reports it; the prefix is checked
        t, ln, seq = struct.unpack_from('<IIQ', b, o)
        end = o + 16 + ln
        if end + 4 > len(b):
            return
        if struct.unpack_from('<I', b, end)[0] != zlib.crc32(b[o:end]):
            raise Bad(f'frame at {o}: CRC')
        if seq != want:
            raise Bad(f'frame at {o}: seq {seq} want {want}')
        want += 1
        yield t, b[o + 16:end]
        o = end + 4


def rstr(p, o):
    n = struct.unpack_from('<H', p, o)[0]
    return p[o + 2:o + 2 + n].decode(), o + 2 + n


def check(base, exp):
    truth = parse_truth(base + '.truth')
    descs, threads, doms = {}, set(), {}
    brackets = 0
    for t, p in frames(open(base + '.prof', 'rb').read(), bool(exp.get('sink_dead'))):
        if t not in KNOWN:
            raise Bad(f'unknown chunk {t}')
        if t == 3:
            n, o = struct.unpack_from('<I', p, 0)[0], 4
            for _ in range(n):
                idx, _id, scope, _pad, _line = struct.unpack_from('<IQBBI', p, o)
                name, o = rstr(p, o + 18)
                _tag, o = rstr(p, o)
                _file, o = rstr(p, o)
                descs[idx] = (name, scope)
        elif t == 4:
            n, o = struct.unpack_from('<I', p, 0)[0], 4
            for _ in range(n):
                inc, _slot, _os = struct.unpack_from('<IIQ', p, o)
                _nm, o = rstr(p, o + 16)
                threads.add(inc)
        elif t == 8:
            n = struct.unpack_from('<I', p, 0)[0]
            did, kind = struct.unpack_from('<IB', p, 4)
            if did in GPU_IDS and n != 1:
                raise Bad(f'GPU DOMAIN chunk with count {n}')
            if did in GPU_IDS:
                if kind != 2:
                    raise Bad(f'domain {did}: kind {kind}')
                if did in doms:
                    raise Bad(f'domain {did} declared twice')
                doms[did] = {'chunks': [], 'final': None, 'after': False}
        elif t in (14, 15, 16, 10):
            did = struct.unpack_from('<I', p, 4 if t == 16 else 0)[0]
            if did not in GPU_IDS:
                continue
            d = doms.get(did)
            if d is None:
                raise Bad(f'chunk {t} for undeclared domain {did}')
            if d['final'] is not None:
                d['after'] = True
            if t == 14:
                _dom, _ep, first, n = struct.unpack_from('<IIQI', p, 0)
                if len(p) != 20 + 28 * n:
                    raise Bad('GPU_EVENTS length')
                evs = []
                for i in range(n):
                    raw, desc, kind, depth, owner, frame = struct.unpack_from('<QIBBxxIQ', p, 20 + 28 * i)
                    if desc not in descs:
                        raise Bad(f'event {first + i}: descriptor {desc} not declared earlier')
                    if owner and owner not in threads:
                        raise Bad(f'event {first + i}: owner {owner} not declared earlier')
                    evs.append((first + i, raw, descs[desc], kind, depth, owner, frame))
                d['chunks'].append(('E', first, n, evs))
            elif t == 15:
                _src, sk, reason, _p, first, count, _r = struct.unpack_from('<IBBHQQI', p, 0)
                d['chunks'].append(('L', first, count, reason))
            elif t == 16:
                if d['final'] is not None:
                    raise Bad(f'domain {did}: two FINALs')
                d['final'] = struct.unpack_from('<IBBHQQQI', p, 4)
            else:
                brackets += struct.unpack_from('<I', p, 16)[0]
    bad, nev, nlost, losses = [], 0, 0, []
    with_events = [did for did, d in doms.items() if d['chunks']]
    if len(with_events) > 1:
        bad.append(f'numbered work in {len(with_events)} domains: the truth names one')
    for did, d in sorted(doms.items()):
        if d['after']:
            bad.append(f'domain {did}: chunks after its FINAL')
        at, last, dev, dlost = 0, 0, 0, 0
        for c in d['chunks']:
            if c[1] != at:
                bad.append(f'domain {did}: tiling at {c[1]}, want {at} (file order)')
                break
            at = c[1] + c[2]
            if c[0] == 'E':
                for s, raw, (dname, scope), kind, depth, owner, frame in c[3]:
                    nev += 1
                    dev += 1
                    if s >= len(truth):
                        bad.append(f'event {s}: no truth')
                        continue
                    tk, td, tdesc, towner, tframe = truth[s]
                    if scope != 2 or dname != tdesc or kind != tk or depth != td or owner != towner or frame != tframe:
                        bad.append(f'event {s}: {(dname, scope, kind, depth, owner, frame)} != truth '
                                   f'{(tdesc, 2, tk, td, towner, tframe)}')
                    if raw < last:
                        bad.append(f'event {s}: timer decreased')
                    last = raw
            else:
                nlost += c[2]
                dlost += c[2]
                losses.append(c[1:])
                if c[1] + c[2] > len(truth):
                    bad.append(f'LOSS [{c[1]}, {c[2]}) beyond the {len(truth)} numbered calls')
        f = d['final']
        dead = bool(exp.get('sink_dead'))
        if not dead and bool(f) != bool(exp['final_present']):
            bad.append(f'domain {did}: FINAL present {bool(f)}, claimed {exp["final_present"]}')
        if f:  # an accepted FINAL is checked on a failed sink's prefix too (its state only when claimed)
            if f[1] != 2 or (not dead and f[2] != exp['final_state']):
                bad.append(f'domain {did}: FINAL kind {f[1]} state {f[2]}, claimed {exp["final_state"]}')
            if f[4] != at:
                bad.append(f'domain {did}: FINAL next_seq {f[4]} != tiling end {at}')
            if f[5] != dev or f[6] != dlost:
                bad.append(f'domain {did}: FINAL events/lost {f[5]}/{f[6]} != delivered {dev}/{dlost}')
            if d['chunks'] and at != len(truth):
                bad.append(f'domain {did}: the frozen frontier {at} != the {len(truth)} numbered calls of the truth')
        if at > len(truth):
            bad.append(f'domain {did}: tiling end {at} beyond the {len(truth)} numbered calls')
    if exp.get('sink_dead'):
        return bad
    if 'domains' in exp and exp['domains'] != len(doms):
        bad.append(f'{len(doms)} GPU domains, claimed {exp["domains"]}')
    if 'domains' not in exp and len(doms) != 1:
        bad.append(f'{len(doms)} GPU domains, claimed 1')
    if exp['events'] != nev:
        bad.append(f'events {nev} != claimed {exp["events"]}')
    if exp['lost'] != nlost:
        bad.append(f'lost {nlost} != claimed {exp["lost"]}')
    if with_events and nev + nlost != len(truth):
        bad.append(f'{nev} events + {nlost} lost != the {len(truth)} numbered calls')
    if 'loss' in exp and exp['loss'] != losses:
        bad.append(f'LOSS list {losses} != claimed {exp["loss"]}')
    if 'loss-chunks' in exp and exp['loss-chunks'] != len(losses):
        bad.append(f'{len(losses)} LOSS chunks != claimed {exp["loss-chunks"]}')
    if 'brackets' in exp and exp['brackets'] != brackets:
        bad.append(f'brackets {brackets} != claimed {exp["brackets"]}')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--suite', required=True, choices=sorted(INVENTORY))
    a = ap.parse_args()
    want = set(INVENTORY[a.suite])
    have = {}
    for f in os.listdir(a.dir):
        stem, ext = os.path.splitext(f)
        if ext in ('.prof', '.truth', '.expect') and stem.startswith('r2-'):
            have.setdefault(stem, set()).add(ext)
    ok = fails = 0
    for stem in sorted(set(have) | want):
        if stem not in want:
            print(f'{stem}: ORACLE FAIL not in the pinned {a.suite} inventory')
            fails += 1
            continue
        miss = {'.prof', '.truth', '.expect'} - have.get(stem, set())
        if miss:
            print(f'{stem}: ORACLE FAIL missing {" ".join(sorted(miss))}')
            fails += 1
            continue
        base = os.path.join(a.dir, stem)
        try:
            bad = check(base, parse_expect(base + '.expect', stem))
        except Bad as x:
            bad = [str(x)]
        except Exception as x:  # fail closed
            bad = [f'oracle error {x!r}']
        if bad:
            fails += 1
            print(f'{stem}: ORACLE FAIL ' + '; '.join(bad[:4]))
        else:
            ok += 1
            print(f'{stem}: oracle PASS')
    print(f'oracle: {ok} pass, {fails} fail ({len(want)} pinned)')
    return 1 if fails or ok != len(want) else 0


if __name__ == '__main__':
    sys.exit(main())
