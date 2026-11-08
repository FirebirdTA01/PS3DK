#!/usr/bin/env python3
"""R4a per-domain checker for MULTI-domain captures: <name>.prof against <name>.dtruth (written by r4a_int.c from
the API notes and the executor's own execution log).  Bounded: it covers what gpu_oracle.py (one numbered domain
per capture) cannot, and nothing else.

dtruth lines (exactly these two kinds):
  domain <id> events <n> lost 0 final_state <s>       (one per domain, in attach order)
  ev <id> <seq> <kind> <depth> <descriptor-name> <raw> (that domain's events, seq contiguous from 0)
The capture must hold EXACTLY the dtruth's GPU domains, each declared once (kind 2).  Per domain, in file order:
GPU_EVENTS tile [0, n) with no LOSS; every event equals its truth row in seq, raw timer, kind, depth and
descriptor name (scope 2, declared earlier); exactly one FINAL after them (kind 2, the state, next_seq n, events n,
lost 0), nothing after it.  Frames: header, CRC, file sequence (gpu_oracle.frames).

usage: r4a_domains.py PROF DTRUTH   (exit 0 consistent, 1 inconsistent, 2 usage/unreadable)"""
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True  # importing the checker must not leave a __pycache__ in the source tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gpu_oracle  # noqa: E402  (frames, rstr, Bad)


def parse_dtruth(path):
    doms, order = {}, []
    for n, l in enumerate(open(path, encoding='ascii'), 1):
        f = l.split()
        if len(f) == 8 and f[0] == 'domain' and f[2] == 'events' and f[4] == 'lost' and f[6] == 'final_state':
            did = int(f[1])
            if did in doms or f[5] != '0':
                raise gpu_oracle.Bad(f'dtruth line {n}: duplicate domain or nonzero lost')
            doms[did] = {'n': int(f[3]), 'state': int(f[7]), 'ev': []}
            order.append(did)
        elif len(f) == 7 and f[0] == 'ev':
            did, seq = int(f[1]), int(f[2])
            d = doms.get(did)
            if d is None or seq != len(d['ev']):
                raise gpu_oracle.Bad(f'dtruth line {n}: event for an undeclared domain or a non-contiguous seq')
            d['ev'].append((seq, int(f[6]), int(f[3]), int(f[4]), f[5]))
        else:
            raise gpu_oracle.Bad(f'dtruth line {n}: unknown shape')
    for did, d in doms.items():
        if len(d['ev']) != d['n']:
            raise gpu_oracle.Bad(f'dtruth domain {did}: {len(d["ev"])} event rows != events {d["n"]}')
    if not doms:
        raise gpu_oracle.Bad('dtruth: no domain')
    return doms


def check(prof, dtruth):
    want = parse_dtruth(dtruth)
    descs, got = {}, {}
    for t, p in gpu_oracle.frames(open(prof, 'rb').read(), False):
        if t == 3:
            n, o = struct.unpack_from('<I', p, 0)[0], 4
            for _ in range(n):
                idx, _id, scope, _pad, _line = struct.unpack_from('<IQBBI', p, o)
                name, o = gpu_oracle.rstr(p, o + 18)
                _tag, o = gpu_oracle.rstr(p, o)
                _file, o = gpu_oracle.rstr(p, o)
                descs[idx] = (name, scope)
        elif t == 8:
            did, kind = struct.unpack_from('<IB', p, 4)
            if did in gpu_oracle.GPU_IDS:
                if did in got or kind != 2:
                    raise gpu_oracle.Bad(f'domain {did}: declared twice or kind {kind}')
                got[did] = {'ev': [], 'final': None, 'after': False, 'loss': 0}
        elif t in (14, 15, 16):
            did = struct.unpack_from('<I', p, 4 if t == 16 else 0)[0]
            if did not in gpu_oracle.GPU_IDS:
                continue
            g = got.get(did)
            if g is None:
                raise gpu_oracle.Bad(f'chunk {t} for undeclared domain {did}')
            if g['final'] is not None:
                g['after'] = True
            if t == 14:
                _dom, _ep, first, n = struct.unpack_from('<IIQI', p, 0)
                if len(p) != 20 + 28 * n or first != len(g['ev']):
                    raise gpu_oracle.Bad(f'domain {did}: GPU_EVENTS length or tiling at {first}')
                for i in range(n):
                    raw, desc, kind, depth, _owner, _frame = struct.unpack_from('<QIBBxxIQ', p, 20 + 28 * i)
                    if desc not in descs:
                        raise gpu_oracle.Bad(f'domain {did} event {first + i}: descriptor not declared earlier')
                    name, scope = descs[desc]
                    g['ev'].append((first + i, raw, kind, depth, name if scope == 2 else f'<scope {scope}>'))
            elif t == 15:
                g['loss'] += 1
            else:
                if g['final'] is not None:
                    raise gpu_oracle.Bad(f'domain {did}: two FINALs')
                g['final'] = struct.unpack_from('<IBBHQQQI', p, 4)
    bad = []
    if sorted(got) != sorted(want):
        bad.append(f'domains {sorted(got)} != dtruth {sorted(want)}')
    for did in sorted(set(got) & set(want)):
        g, w = got[did], want[did]
        if g['loss']:
            bad.append(f'domain {did}: {g["loss"]} LOSS chunks, dtruth none')
        if g['after']:
            bad.append(f'domain {did}: chunks after its FINAL')
        if g['ev'] != w['ev']:
            k = next((i for i, (a, b) in enumerate(zip(g['ev'], w['ev'])) if a != b), min(len(g['ev']), len(w['ev'])))
            bad.append(f'domain {did}: events differ at {k}: {g["ev"][k:k + 1]} != dtruth {w["ev"][k:k + 1]} '
                       f'({len(g["ev"])} vs {len(w["ev"])})')
        f = g['final']
        if f is None:
            bad.append(f'domain {did}: no FINAL')
        elif f[1] != 2 or f[2] != w['state'] or f[4] != w['n'] or f[5] != w['n'] or f[6] != 0:
            bad.append(f'domain {did}: FINAL kind/state/next/events/lost {f[1]}/{f[2]}/{f[4]}/{f[5]}/{f[6]} != '
                       f'2/{w["state"]}/{w["n"]}/{w["n"]}/0')
    return bad


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    try:
        bad = check(sys.argv[1], sys.argv[2])
    except gpu_oracle.Bad as x:
        bad = [str(x)]
    except (OSError, struct.error, ValueError) as x:
        print(f'DOMAINS FAIL unreadable: {x!r}')
        return 2
    for b in bad:
        print(f'DOMAINS FAIL {b}')
    if not bad:
        print('DOMAINS PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
