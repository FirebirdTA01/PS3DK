#!/usr/bin/env python3
"""Independent integer checker for the SPU ticks reader (spu-ticks slice design 10.8, delta D1.4, C1-9).

It shares no code with the reader: it decodes each capture file itself (chunk walk, the extension chunks and the
SPU_ORDER records), pairs the zones itself, and recomputes from the decoded records, by the DIRECT definitions of
the design (every admitted bound, every i < j), every observation's [L, U] (with Upper-1 and Upper-2), every step
classification, the segments, the tick counts, the per-segment availability reasons, the pair classes, the
placement states and the coverage counts Q and q_p.  It runs examples/spu_ticks_json on the same files and compares
every field.  Files the reader refuses (MALFORMED) are skipped by both and counted.

usage: spu_ticks_check.py <spu_ticks_json executable> <fixture dir> [--self-test]
exit: 0 every file agrees (with --self-test also: every corruption is caught); 1 a disagreement; 2 tool failure.
--self-test: corrupts one expected value per field kind and requires the comparison to FAIL on each, so the
comparison is not vacuous."""
import json
import os
import struct
import subprocess
import sys

RMAX = 2
U32 = 1 << 32


def decode(data):
    """The extension records of a capture, or None when it is not a flags-7 file."""
    if len(data) < 16 or data[:8] != b'PS3DKPRF':
        return None
    flags = struct.unpack_from('<I', data, 12)[0]
    if flags != 7:
        return None
    pos, trailer = 16, False
    lanes = {}       # lane -> domain
    dom_lane = {}    # domain -> lane
    epochs = {}      # domain -> epoch count
    ev = {}          # lane -> [(seq, epoch, raw, desc, kind, depth)]
    hist = {}        # lane -> pairing state
    brackets = {}    # domain -> {(epoch, seq): (raw, tb0, tb1)}
    losses = {}      # lane -> [(first, count)]
    finals = {}      # lane -> (state, open)
    order = {}       # lane -> dict
    while pos + 16 <= len(data):
        ty, ln, _ = struct.unpack_from('<IIQ', data, pos)
        if pos + 16 + ln + 4 > len(data):
            break
        p = data[pos + 16:pos + 16 + ln]
        pos += 16 + ln + 4
        if ty == 6:
            trailer = True
        elif ty == 12:
            n = struct.unpack_from('<I', p, 0)[0]
            o = 4
            for _ in range(n):
                lane, dom = struct.unpack_from('<II', p, o)
                o += 24
                sl = struct.unpack_from('<H', p, o)[0]
                o += 2 + sl
                lanes[lane] = dom
                dom_lane[dom] = lane
                ev[lane], losses[lane] = [], []
                hist[lane] = dict(known=True, ever=False, proto=False, stack=[], pairs=[], kept=False)
                order.setdefault(lane, dict(created=None, at=[], drains=[], final=None))
        elif ty == 9:
            n = struct.unpack_from('<I', p, 0)[0]
            for k in range(n):
                dom, e, reason = struct.unpack_from('<IIB', p, 4 + 32 * k)
                epochs[dom] = e
                lane = dom_lane.get(dom)
                if lane is not None and e > 1:
                    h = hist[lane]
                    if reason == 2 and h['known'] and not h['ever'] and not h['proto'] and not h['stack']:
                        h['kept'] = True
                    else:
                        h['stack'], h['known'], h['ever'] = [], False, True
        elif ty == 10:
            dom, e, first, n = struct.unpack_from('<IIQI', p, 0)
            for k in range(n):
                f, t0, t1 = struct.unpack_from('<QQQ', p, 20 + 24 * k)
                brackets.setdefault(dom, {})[(e, first + k)] = (f, t0, t1)
        elif ty == 13:
            lane, e, first, n = struct.unpack_from('<IIQI', p, 0)
            h = hist[lane]
            for k in range(n):
                raw, desc, kind, depth = struct.unpack_from('<IHBB', p, 20 + 8 * k)
                seq = first + k
                ev[lane].append((seq, e, raw, desc, kind, depth))
                if h['proto']:
                    continue
                if not h['known']:
                    if kind == 1 and depth == 0:
                        h['known'] = True
                        h['stack'].append((desc, 0, seq))
                    continue
                if kind == 1:
                    if depth != len(h['stack']):
                        h['proto'] = True
                    else:
                        h['stack'].append((desc, depth, seq))
                elif kind == 3:
                    if depth != len(h['stack']):
                        h['proto'] = True
                elif h['stack'] and h['stack'][-1][:2] == (desc, depth):
                    h['pairs'].append((h['stack'].pop()[2], seq))
                else:
                    h['proto'] = True
        elif ty == 15:
            src, kind = struct.unpack_from('<IB', p, 0)
            first, count = struct.unpack_from('<QQ', p, 8)
            if kind == 1:
                losses[src].append((first, count))
                h = hist[src]
                h['stack'], h['known'], h['ever'] = [], False, True
        elif ty == 16:
            n = struct.unpack_from('<I', p, 0)[0]
            for k in range(n):
                src, kind, state = struct.unpack_from('<IBB', p, 4 + 36 * k)
                op = struct.unpack_from('<I', p, 4 + 36 * k + 32)[0]
                if kind == 1:
                    finals[src] = (state, op)
        elif ty == 18:
            n = struct.unpack_from('<I', p, 0)[0]
            recs = [p[8 + 24 * k:32 + 24 * k] for k in range(n)]
            kind, lane = struct.unpack_from('<II', recs[0], 0)
            od = order[lane]
            if kind == 1:
                e, b, before = struct.unpack_from('<IIQ', recs[0], 8)
                od['at'].append((e, b, before))
            elif kind == 2:
                e = struct.unpack_from('<I', recs[0], 8)[0]
                through = struct.unpack_from('<Q', recs[0], 16)[0]
                tb = struct.unpack_from('<Q', recs[1], 8)[0]
                od['drains'].append((e, through, tb))
            elif kind == 4:
                od['created'] = struct.unpack_from('<Q', recs[0], 8)[0]
            elif kind == 5:
                od['final'] = struct.unpack_from('<QQ', recs[0], 8)
    return dict(lanes=lanes, epochs=epochs, ev=ev, hist=hist, brackets=brackets, losses=losses, finals=finals,
                order=order, trailer=trailer)


def expected(d):
    out_lanes, Q, qp = [], 0, 0
    for lane in sorted(d['lanes']):
        dom = d['lanes'][lane]
        od = d['order'][lane]
        events = d['ev'][lane]
        nep = d['epochs'].get(dom, 0)
        h = d['hist'][lane]
        lost = sum(c for _, c in d['losses'][lane])
        fin = d['finals'].get(lane)
        known = not h['ever'] and not h['proto']
        placeholder = known and h['kept'] and fin is not None and fin[1] == 0xFFFFFFFF
        raw_ok = (d['trailer'] and fin is not None and fin[0] == 1 and lost == 0 and not h['proto']
                  and known and not placeholder)
        br = d['brackets'].get(dom, {})
        at = od['at']
        named = {(e, b) for e, b, _ in at}
        unpos = {}
        for (e, b) in br:
            if (e, b) not in named:
                unpos[e] = unpos.get(e, 0) + 1
        # program order: a bracket with events_before = k precedes every event with seq >= k
        obs, i = [], 0
        for e, b, before in at:
            while i < len(events) and events[i][0] < before:
                obs.append(('e',) + events[i])
                i += 1
            raw, t0, t1 = br[(e, b)]
            obs.append(('b', b, e, raw, t0, t1, before))
        obs += [('e',) + x for x in events[i:]]
        n = len(obs)
        # the direct definitions of the bounds
        L, U1, U2, U = [], [], [], []
        for k, o in enumerate(obs):
            lows = [od['created']] if od['created'] is not None else []
            lows += [p[4] for p in obs[:k + 1] if p[0] == 'b']
            L.append(max(lows) if lows else None)
            u1 = None
            if o[0] == 'e':
                cand = [tb for _, through, tb in od['drains'] if through > o[1]]
                u1 = cand[0] if cand else None
            ups2 = [p[5] for p in obs[k:] if p[0] == 'b']
            u2 = min(ups2) if ups2 else None
            U1.append(u1)
            U2.append(u2)
            both = [v for v in (u1, u2) if v is not None]
            U.append(min(both) if both else None)
        # order contradictions: any i < j with L_i > U_j (all pairs); the epochs marked are those of o_{j-1}, o_j
        contra, oc = set(), 0
        tbs = [tb for _, _, tb in od['drains']]
        if any(tbs[k] < tbs[k - 1] for k in range(1, len(tbs))):
            contra = set(range(1, nep + 1))
            oc += 1
        for j in range(1, n):
            if U[j] is not None and any(L[i] is not None and L[i] > U[j] for i in range(j)):
                if L[j - 1] is not None and L[j - 1] > U[j]:
                    contra |= {epoch_of(obs[j - 1]), epoch_of(obs[j])}
                    oc += 1
        lost_seqs = set()
        for f, c in d['losses'][lane]:
            lost_seqs |= set(range(f, f + c))
        # steps and segments
        segs, seg_of, f = [], [], []
        dcount = 0
        for k in range(n):
            if k == 0:
                segs.append([epoch_of(obs[0]), 0, 0, 0, None, 'end of the lane'])
                seg_of.append(0)
                f.append(0)
                continue
            a, b = obs[k - 1], obs[k]
            lo = next_seq(a)
            hi = first_seq(b)
            if epoch_of(a) != epoch_of(b):
                end = 'epoch boundary'
            elif any(s in lost_seqs for s in range(lo, hi)):
                end = 'LOSS'
            else:
                m = (raw_of(a) - raw_of(b)) % U32
                end = step(m, L[k - 1], U[k])
                if end == 'D contradicted':
                    dcount += 1
            if end == 'PROVEN':
                s = segs[-1]
                s[2] = k
                s[3] += 1
                seg_of.append(len(segs) - 1)
                f.append(f[-1] + m)
            else:
                segs[-1][5] = end
                segs.append([epoch_of(b), k, k, 0, None, 'end of the lane'])
                seg_of.append(len(segs) - 1)
                f.append(0)
        dropped = od['final'][1] if od['final'] is not None else 0
        t2 = []
        if od['final'] is None:
            t2.append('order coverage unknown (no ORDER_FINAL)')
        if not raw_ok:
            t2.append('raw validity')
        for s in segs:
            un = list(t2)
            if s[0] in contra:
                un.append('order contradiction')
            if dropped > 0:
                un.append('order coverage')
            elif unpos.get(s[0], 0) > 0:
                un.append('order coverage (unpositioned bracket)')
            s[4] = un or None
        # pairs (same epoch only)
        ev_obs = {o[1]: k for k, o in enumerate(obs) if o[0] == 'e'}
        ep = {x[0]: x[1] for x in events}
        pairs = []
        for b, e in h['pairs']:
            if ep[b] != ep[e]:
                continue
            kb, ke = ev_obs[b], ev_obs[e]
            sb, se = seg_of[kb], seg_of[ke]

            def place(k):
                if t2:
                    return 'UNPLACED'
                if epoch_of(obs[k]) in contra:
                    return 'UNPLACED'
                return {(True, True): 'PLACED', (True, False): 'LOWER-ONLY', (False, True): 'UPPER-ONLY',
                        (False, False): 'UNPLACED'}[(L[k] is not None, U[k] is not None)]
            ticks, cls = None, None
            if t2:
                cls = f'segment TICKS-UNAVAILABLE ({t2[0]})'
            elif ep[b] in contra:
                cls = 'segment TICKS-UNAVAILABLE (order contradiction)'
            elif dropped > 0 or unpos.get(ep[b], 0) > 0:
                cls = 'segment TICKS-UNAVAILABLE (order coverage)'
            elif sb != se:
                cls = f'cross-segment ({segs[sb][5]})'
            else:
                ticks = str(f[ke] - f[kb])
                cls = 'segment TICKS-CONDITIONAL'
            pb, pe = place(kb), place(ke)
            pairs.append([b, e, ticks, cls, pb, pe])
            Q += 1
            qp += int(pb == pe == 'PLACED')
        obs_out = []
        for k, o in enumerate(obs):
            if o[0] == 'e':
                obs_out.append(['e', o[2], o[1], o[3], L[k], U1[k], U2[k], U[k], seg_of[k]])
            else:
                obs_out.append(['b', o[2], o[1], o[3], L[k], U1[k], U2[k], U[k], seg_of[k]])
        state = 'EVALUATED' if obs else ('EMPTY' if not t2 else 'EMPTY-UNPROVEN')
        out_lanes.append({'lane': lane, 'state': state, 'obs': obs_out, 'segs': segs, 'pairs': pairs, 'd': dcount,
                          'oc': oc})
    return {'lanes': out_lanes, 'cov': [Q, 0, qp]}


def epoch_of(o):
    return o[2]


def raw_of(o):
    return o[3]


def first_seq(o):
    return o[1] if o[0] == 'e' else o[6]


def next_seq(o):
    return o[1] + 1 if o[0] == 'e' else o[6]


def step(m, lo, up):
    if lo is None or up is None:
        return 'no bound'
    if lo > up:
        return 'order contradiction'
    bound = RMAX * (up - lo)
    if m > bound:
        return 'D contradicted'
    if bound >= U32:
        return 'long gap'
    return 'PROVEN'


def compare(want, got):
    diffs = []
    if got['cov'] != want['cov']:
        diffs.append(f"cov {got['cov']} != {want['cov']}")
    if len(got['lanes']) != len(want['lanes']):
        return diffs + ['lane count']
    for gw, ww in zip(got['lanes'], want['lanes']):
        for k in ('lane', 'state', 'obs', 'segs', 'pairs', 'd', 'oc'):
            if gw[k] != ww[k]:
                diffs.append(f"lane {ww['lane']} {k}")
    return diffs


def main():
    exe, fixdir = sys.argv[1], sys.argv[2]
    self_test = '--self-test' in sys.argv[3:]
    files = sorted(os.path.join(fixdir, n) for n in os.listdir(fixdir) if n.endswith('.prof'))
    if not files:
        print('no fixtures')
        return 2
    got = {}
    for k in range(0, len(files), 200):
        r = subprocess.run([exe, *files[k:k + 200]], capture_output=True, text=True)
        if r.returncode != 0:
            print(f'tool failure rc {r.returncode}: {r.stderr[:400]}')
            return 2
        for line in r.stdout.splitlines():
            o = json.loads(line)
            got[o['file']] = o
    checked = skipped = malformed = 0
    bad = []
    cases = []
    for fpath in files:
        g = got[fpath]
        if g['malformed']:
            malformed += 1
            continue
        d = decode(open(fpath, 'rb').read())
        if d is None or not g.get('order'):
            skipped += 1
            continue
        w = expected(d)
        cases.append((w, g))
        diffs = compare(w, g)
        checked += 1
        if diffs:
            bad.append(f'{os.path.basename(fpath)}: {diffs[:4]}')
    print(f'files {len(files)}: checked {checked}, reader MALFORMED {malformed}, skipped (no order records) {skipped}')
    for b in bad[:20]:
        print(f'DIFF {b}')
    print(f"RESULT {'AGREE' if checked and not bad else 'DISAGREE'} ({len(bad)} files differ)")
    rc = 0 if checked and not bad else 1
    if self_test and rc == 0:
        caught = []
        kinds = {
            'L': lambda w: set_obs(w, 4), 'U1': lambda w: set_obs(w, 5), 'U2': lambda w: set_obs(w, 6),
            'U': lambda w: set_obs(w, 7), 'segment index': lambda w: set_obs(w, 8),
            'step class': lambda w: set_seg(w, 5, 'no bound'), 'steps proven': lambda w: set_seg(w, 3, 999),
            'reasons': lambda w: set_seg(w, 4, ['raw validity']), 'ticks': lambda w: set_pair(w, 2, '7'),
            'pair class': lambda w: set_pair(w, 3, 'cross-segment (LOSS)'), 'placement': lambda w: set_pair(w, 4, 'X'),
            'Q': lambda w: w['cov'].__setitem__(0, w['cov'][0] + 1) or True,
            'q_p': lambda w: w['cov'].__setitem__(2, w['cov'][2] + 1) or True,
            'D count': lambda w: set_lane(w, 'd'), 'order count': lambda w: set_lane(w, 'oc'),
        }
        for name, damage in kinds.items():
            hit = False
            for w, g in cases:
                w2 = json.loads(json.dumps(w))
                if damage(w2) and compare(w2, g):
                    hit = True
                    break
            caught.append((name, hit))
            print(f"self-test {name}: {'CAUGHT' if hit else 'MISSED'}")
        if not all(h for _, h in caught):
            rc = 1
    return rc


def set_obs(w, i):
    for ln in w['lanes']:
        if ln['obs']:
            v = ln['obs'][0][i]
            ln['obs'][0][i] = (v or 0) + 1
            return True
    return False


def set_seg(w, i, v):
    for ln in w['lanes']:
        if ln['segs']:
            ln['segs'][0][i] = v
            return True
    return False


def set_pair(w, i, v):
    for ln in w['lanes']:
        if ln['pairs']:
            ln['pairs'][0][i] = v
            return True
    return False


def set_lane(w, k):
    if w['lanes']:
        w['lanes'][0][k] += 1
        return True
    return False


if __name__ == '__main__':
    sys.exit(main())
