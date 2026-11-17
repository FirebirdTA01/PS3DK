"""Independent check of a REPORT_RING (2047/11) AUTO capture, written from the wire description in auto_wire.h, not
from the reader. verify() raises AssertionError on any violation and returns a summary."""
import struct
import zlib

# COVERAGE columns 2..11, in wire order
EXCLUSIONS = ('room', 'report capacity', 'metadata capacity', 'reentry', 'unsupported arguments', 'shutdown',
              'ownership uncertainty', 'observer disabled', 'lease unverified', 'epoch invalid')


def chunks(data):
    assert data[:8] == b'PS3DKPRF'
    pos, seq, out = 16, 0, []
    while pos < len(data):
        kind, n, s = struct.unpack_from('<IIQ', data, pos)
        assert s == seq and n <= len(data) - pos - 20
        assert struct.unpack_from('<I', data, pos + 16 + n)[0] == zlib.crc32(data[pos:pos + 16 + n]) & 0xffffffff
        out.append((kind, data[pos + 16:pos + 16 + n]))
        seq += 1
        pos += 20 + n
    return out


def config(b):
    assert len(b) == 216 and struct.unpack_from('<HH', b) == (1, 0), 'ring CONFIG width/refusal'
    c = dict(zip(('masks', 'first', 'count', 'op_label', 'clock_label', 'pairs', 'metadata', 'frames', 'width'),
                 ((struct.unpack_from('<II', b, 24)),) + struct.unpack_from('<8I', b, 32)))
    assert c['masks'] == (2047, 11) and c['width'] == 176
    r = dict(zip(('bytes', 'blocks', 'pairs', 'first', 'label', 'stride', 'slots', 'metadata_stride', 'legacy',
                  'owner_only'), struct.unpack_from('<10I', b, 176)))
    assert r['owner_only'] == 1 and 2 <= r['blocks'] <= 4 and r['pairs'] == 126
    assert r['first'] == c['first'] + c['count'] - 2 * r['blocks'] * r['pairs'], 'ring reports end the range'
    assert r['legacy'] == c['pairs'] - r['blocks'] * r['pairs'] and r['legacy'] > 0, 'report partition'
    assert r['first'] >= c['first'] + 16 + 2 * r['legacy'], 'ring blocks overlap the legacy pool'
    assert r['label'] not in (c['op_label'], c['clock_label']) and r['stride'] >= 32 * r['pairs']
    assert r['slots'] == r['bytes'] // r['stride'] and r['bytes'] % (1 << 20) == 0
    return c, r


def record(q):
    v = dict(id=struct.unpack_from('<Q', q, 0)[0], owner=struct.unpack_from('<I', q, 8)[0],
             kind=struct.unpack_from('<H', q, 12)[0], flags=struct.unpack_from('<H', q, 14)[0],
             frame=struct.unpack_from('<Q', q, 16)[0], raw=struct.unpack_from('<QQ', q, 40),
             segment=struct.unpack_from('<I', q, 92)[0], reports=struct.unpack_from('<II', q, 96),
             serials=struct.unpack_from('<II', q, 104), published=struct.unpack_from('<Q', q, 120)[0],
             gen=struct.unpack_from('<I', q, 160)[0], slot=struct.unpack_from('<I', q, 164)[0],
             ordinal=struct.unpack_from('<H', q, 168)[0], local=struct.unpack_from('<H', q, 170)[0],
             state=struct.unpack_from('<I', q, 172)[0])
    return v


def ops(data):
    out = []
    for k, b in chunks(data):
        if k == 35:
            out += [record(b[16 + 176 * i:16 + 176 * (i + 1)]) for i in range(struct.unpack_from('<I', b, 8)[0])]
    return out


def verify(data, pending_tail=0, held_extents=None):
    """pending_tail: the trailing ring records left incomplete; held_extents: the extents held at seal (default 1
    with a pending tail, else 0); any held extent requires LOWER_BOUND."""
    auto = [(k, b) for k, b in chunks(data) if 34 <= k <= 38]
    assert auto and auto[0][0] == 34
    c, r = config(auto[0][1])
    ops, rows, status = [], [], None
    for k, b in auto[1:]:
        if k == 35:
            assert struct.unpack_from('<I', b, 12)[0] == 176, 'OPS width'
            n = struct.unpack_from('<I', b, 8)[0]
            assert len(b) == 16 + 176 * n
            ops += [record(b[16 + 176 * i:16 + 176 * (i + 1)]) for i in range(n)]
        elif k == 36:
            assert len(b) == 528, 'COVERAGE width'
            rows.append(b)
        elif k == 37:
            assert len(b) == 208, 'STATUS width'
            status = b
    assert status is not None
    ring = [o for o in ops if o['flags'] & 128]
    for o in ops:
        if not o['flags'] & 128:
            assert (o['gen'], o['slot'], o['ordinal'], o['local'], o['state']) == (0, 0, 0, 0, 0), 'forged ring tail'
    prev_gen = prev_ord = 0
    last_raw = 0
    tokens = {}
    timed_by_frame = {}
    for o in ring:
        g = o['gen']
        assert o['kind'] in (1, 2, 3) and not o['flags'] & 64 and o['segment'] == 0, 'ring provenance'
        assert g >= 1 and o['slot'] == (g - 1) % r['slots'] and o['local'] == (g - 1) % r['blocks'], 'ring slot'
        assert o['ordinal'] < r['pairs'] and o['serials'] == (2 * g - 1, 2 * g), 'ring serials'
        assert o['reports'] == (r['first'] + 2 * r['pairs'] * o['local'] + 2 * o['ordinal'],
                                r['first'] + 2 * r['pairs'] * o['local'] + 2 * o['ordinal'] + 1), 'ring reports'
        if g == prev_gen:
            assert o['ordinal'] == prev_ord + 1, 'ordinal'
        else:
            assert g == prev_gen + 1 and o['ordinal'] == 0, 'generation'
        token = (o['flags'] & 1, o['owner'], o['frame'])
        assert tokens.setdefault(g, token) == token, 'a block spans frames or windows'
        prev_gen, prev_ord = g, o['ordinal']
        assert o['state'] in (0, 1)
        if o['flags'] & 2:
            assert not o['flags'] & 4 and o['raw'][0] < o['raw'][1] and o['raw'][0] > last_raw, 'raw order'
            # the ring label read may be odd: a later block's copy can be in flight; it is never below this block's
            assert o['published'] >= 2 * g, 'publication'
            last_raw = o['raw'][1]
            if o['flags'] & 1:
                timed_by_frame[o['frame']] = timed_by_frame.get(o['frame'], 0) + 1
        else:
            assert o['flags'] & 4 and o['raw'] == (0, 0)
    if pending_tail:
        assert all(o['flags'] & 4 and o['flags'] & 32 and not o['flags'] & 2 for o in ring[-pending_tail:])
        assert all(o['flags'] & 2 for o in ring[:-pending_tail]), 'only the block pending at seal is incomplete'
    else:
        assert all(o['flags'] & 2 for o in ring), 'every ring record timed'
    excluded = dict.fromkeys(EXCLUSIONS + ('ring_full', 'ring_state', 'outside'), 0)
    observed_by_frame = {}
    for b in rows:
        flags, frame = struct.unpack_from('<H', b, 2)[0], struct.unpack_from('<Q', b, 16)[0]
        for kind in range(3):
            counts = struct.unpack_from('<12Q', b, 64 + 96 * kind)
            outside = struct.unpack_from('<Q', b, 416 + 8 * kind)[0]
            full = struct.unpack_from('<Q', b, 480 + 8 * kind)[0]
            state = struct.unpack_from('<Q', b, 504 + 8 * kind)[0]
            if not flags & 4:
                assert counts[0] == counts[1] + sum(counts[2:]) + outside + full + state, 'coverage conservation'
            for name, n in zip(EXCLUSIONS, counts[2:]):
                excluded[name] += n
            excluded['ring_full'] += full
            excluded['ring_state'] += state
            excluded['outside'] += outside
            observed_by_frame[frame] = observed_by_frame.get(frame, 0) + counts[1]
    flags = struct.unpack_from('<H', status, 2)[0]
    tail = struct.unpack_from('<8Q', status, 128)
    issued, retired, published, held, copied, untouched, restored, decoded = tail
    assert flags & 1 and held == issued - retired and issued == prev_gen, 'STATUS ring tail'
    assert published % 2 == 0 and published <= 2 * issued and untouched + restored <= issued
    assert copied <= issued * 32 * r['pairs']
    if held_extents is None:
        held_extents = 1 if pending_tail else 0
    assert held == held_extents, 'held extents at seal'
    if held:
        assert flags & 2, 'a held extent at seal is a lower bound'
    else:
        assert retired == issued
    header, at, reason, reserved = struct.unpack_from('<4I', status, 192)
    assert reserved == 0 and reason <= 16 and (reason or (header, at) == (0, 0)), 'decoder rejection'
    return dict(ring_records=len(ring), blocks_issued=issued, blocks_retired=retired, held=held,
                copied_bytes=copied, flushes_untouched=untouched, flushes_restored=restored, decoded_words=decoded,
                rejected=(header, at, reason),
                max_timed_per_frame=max(timed_by_frame.values()) if timed_by_frame else 0,
                frames_over_504=sum(1 for v in timed_by_frame.values() if v > 504),
                exclusions={k: v for k, v in excluded.items() if v}, status_flags=flags, frames=len(rows))
