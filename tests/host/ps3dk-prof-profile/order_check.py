#!/usr/bin/env python3
"""SPU order records in a capture written by the real runtime (spu-ticks slice delta D3.1-D3.4), decoded here
independently of the reader: file_flags must be 7; every SPU_ORDER chunk (type 18) is n = 1, or n = 2 holding
DRAINED then DRAIN_TB with the same lane and events_through; and every declared SPU lane has exactly one
LANE_CREATED before its other order records, at least one DRAINED + DRAIN_TB pair, and exactly one ORDER_FINAL in the
chunk immediately before the DOMAIN_FINAL chunk that finalizes the lane, whose records_emitted equals the lane's
order records of kinds 1-4.  usage: order_check.py FILE   (exit 0 PASS, 1 FAIL)"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
bad = []
flags = struct.unpack_from('<I', data, 12)[0]
if flags != 7:
    bad.append(f'file_flags {flags:#x}, want 0x7')
pos, chunks = 16, []
while pos + 16 <= len(data):
    ty, ln, _ = struct.unpack_from('<IIQ', data, pos)
    chunks.append((ty, data[pos + 16:pos + 16 + ln]))
    pos += 16 + ln + 4
lanes, per = [], {}
for i, (ty, p) in enumerate(chunks):
    if ty == 12:
        for k in range(struct.unpack_from('<I', p, 0)[0]):
            off = 4
            for _ in range(k):
                off += 24 + 2 + struct.unpack_from('<H', p, off + 24)[0]
            lanes.append(struct.unpack_from('<I', p, off)[0])
    elif ty == 18:
        n = struct.unpack_from('<I', p, 0)[0]
        recs = [struct.unpack_from('<IIQQ', p, 8 + 24 * k) for k in range(n)]
        if n == 2:
            if recs[0][0] != 2 or recs[1][0] != 3 or recs[0][1] != recs[1][1] or recs[0][3] != recs[1][3]:
                bad.append(f'chunk {i}: a two-record chunk that is not DRAINED then its DRAIN_TB')
        elif n != 1 or recs[0][0] in (2, 3):
            bad.append(f'chunk {i}: n {n} kind {recs[0][0]}')
        for kind, lane, a, b in recs:
            per.setdefault(lane, []).append((i, kind, a, b))
    elif ty == 16:
        for k in range(struct.unpack_from('<I', p, 0)[0]):
            src, kind = struct.unpack_from('<IB', p, 4 + 36 * k)
            if kind == 1:
                rec = per.get(src, [])
                fin = [r for r in rec if r[1] == 5]
                if len(fin) != 1 or fin[0][0] != i - 1:
                    bad.append(f'lane {src}: ORDER_FINAL not the chunk right before its DOMAIN_FINAL ({fin})')
                elif fin[0][2] != sum(1 for r in rec if r[1] in (1, 2, 3, 4)):
                    bad.append(f'lane {src}: records_emitted {fin[0][2]} != {sum(1 for r in rec if r[1] < 5)}')
if not lanes:
    bad.append('no SPU lane declared')
for lane in lanes:
    rec = per.get(lane, [])
    kinds = [r[1] for r in rec]
    if kinds.count(4) != 1 or kinds[0] != 4:
        bad.append(f'lane {lane}: LANE_CREATED not exactly once and first ({kinds[:4]})')
    if kinds.count(2) < 1 or kinds.count(2) != kinds.count(3):
        bad.append(f'lane {lane}: DRAINED {kinds.count(2)} DRAIN_TB {kinds.count(3)}')
    if kinds.count(5) != 1:
        bad.append(f'lane {lane}: ORDER_FINAL {kinds.count(5)} times')
    print(f'lane {lane}: LANE_CREATED {kinds.count(4)} BRACKET_AT {kinds.count(1)} DRAINED {kinds.count(2)} '
          f'DRAIN_TB {kinds.count(3)} ORDER_FINAL {kinds.count(5)}')
for b in bad:
    print(f'FAIL {b}')
print('order_check:', 'FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
