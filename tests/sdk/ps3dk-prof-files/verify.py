#!/usr/bin/env python3
"""Check fixture counts and wire ABI/drain mode after production reader success."""
import argparse
import json
from pathlib import Path
import struct


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--abi', choices=['ilp32', 'lp64'], required=True)
    p.add_argument('--mode', choices=['app', 'thread'], required=True)
    p.add_argument('--arcs-json', type=Path, required=True)
    p.add_argument('--timing', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    a = p.parse_args()
    raw = a.arcs_json.read_bytes()
    capture = json.loads(raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'))
    workers = 4 if a.mode == 'app' else 1
    abi = 1 if a.abi == 'ilp32' else 2
    consumer = 1 if a.mode == 'app' else 2
    identity = capture['identity']
    checks = []

    def check(name, ok):
        checks.append(bool(ok))
        print(f'{name}: {"PASS" if ok else "FAIL"}')

    # Arc format HEADER mode 2 means paired timing+arcs, NOT THREAD drain mode.
    check('arc-identity', capture['complete'] is True and identity['abi'] == abi
          and identity['mode'] == 2 and identity['final_snapshot'] is True
          and identity['max_threads'] == 1 and identity['max_arcs_per_thread'] == 4
          and identity['workload_tag'] == a.mode)
    threads, arcs, finals = capture['threads'], capture['arcs'], capture['finals']
    check('incarnations', len(threads) == workers
          and len({t['incarnation'] for t in threads}) == workers
          and all(t['slot'] == 0 and t['name'] == 'file-worker' for t in threads))
    incarnations = {t['incarnation'] for t in threads}
    check('arc-counts', len(arcs) == workers and {r['incarnation'] for r in arcs} == incarnations
          and all(r['count'] == '2' and r['caller'] == '0x0000000000000004'
                  and r['callee'] == '0x0000000000000008' and r['unknown_caller'] is False for r in arcs))
    check('retired-counts', len(finals) == workers and {f['incarnation'] for f in finals} == incarnations
          and all(f['state'] == 1 and f['limit_flags'] == 0 and f['attempted'] == '2'
                  and f['recorded'] == '2' and f['refused'] == '0' and f['lost'] == '0' for f in finals))
    # Frozen timing format: 16-byte file header, 16-byte chunk header, six
    # u16-length strings, then ABI and consumer_mode. Production check-pair/report
    # must already have validated structure and CRCs; this is an independent
    # check of the two expected fixture values, not another capture validator.
    timing = a.timing.read_bytes()
    pos = 32
    try:
        kind, length, sequence = struct.unpack_from('<IIQ', timing, 16)
        end = pos + length
        if kind != 1 or sequence != 0 or end + 4 > len(timing):
            raise ValueError('invalid HEADER')
        for _ in range(6):
            n, = struct.unpack_from('<H', timing, pos)
            pos += 2 + n
            if pos > end:
                raise ValueError('string beyond HEADER')
        ok = pos + 2 <= end and timing[pos] == abi and timing[pos + 1] == consumer
    except (ValueError, struct.error, IndexError):
        ok = False
    check('timing-abi-and-consumer', ok)
    probe = a.probe.read_bytes()
    check('retained-sentinel', probe == bytes(i * 17 & 255 for i in range(65537)))
    passed = sum(checks)
    print(f'fixture checks tests={len(checks)} pass={passed} fail={len(checks)-passed}')
    return 0 if all(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
