#!/usr/bin/env python3
"""Host comparator for the rig's float render-target mode (rt=f32).

Judges two raw readbacks of a CELL_GCM_SURFACE_F_W32Z32Y32X32 colour
target: RT_W x RT_H pixels, four big-endian binary32 lanes per pixel in
guest memory order, rows `pitch` bytes apart.  The verdict rules are the
reviewed design's (docs/local/design-rig-float-target-2026-10-19.md, codex
2026-10-19 15:34):

  - the primary result is the RAW bit comparison per lane;
  - a lane NaN on both sides is a separate NON-PASS category (paired-nan),
    even when the payload bits match; differing payloads are counted;
  - NaN on one side only is a difference, as is +0 vs -0;
  - ULP distance is computed for finite pairs only and is diagnostic,
    never a tolerance;
  - per-lane finite min/max exclude non-finites; finite/NaN/Inf counts are
    reported so a flat or blind frame is visible.

Row status: invalid-readback (wrong size), mismatch (any raw difference
that is not a paired NaN), paired-nan (no other difference, but a required
lane is NaN on both sides somewhere), identical.

usage: f32_compare.py A.raw B.raw [--pitch N] [--lanes xyzw]   (JSON on stdout)
       f32_compare.py --self-test
"""
import json
import math
import struct
import sys

RT_W = RT_H = 64
PIXEL_BYTES = 16
LANES = 'xyzw'


def _words(raw, pitch):
    need = pitch * (RT_H - 1) + RT_W * PIXEL_BYTES
    if pitch < RT_W * PIXEL_BYTES or len(raw) < need:
        raise ValueError('readback is %d bytes, needs %d (pitch %d)' % (len(raw), need, pitch))
    rows = []
    for y in range(RT_H):
        base = y * pitch
        rows.append(struct.unpack_from('>%dI' % (RT_W * 4), raw, base))
    return rows


def _is_nan(w):
    return (w & 0x7F800000) == 0x7F800000 and (w & 0x007FFFFF) != 0


def _is_inf(w):
    return (w & 0x7FFFFFFF) == 0x7F800000


def _f(w):
    return struct.unpack('>f', struct.pack('>I', w))[0]


def _ordered(w):
    # map binary32 bits onto a monotonic integer line (for finite ULP distance)
    return (0x80000000 - (w & 0x7FFFFFFF)) if w & 0x80000000 else (0x80000000 + w)


def judge(raw_a, raw_b, pitch=RT_W * PIXEL_BYTES, lanes=(0, 1, 2, 3), first_n=8):
    try:
        wa, wb = _words(raw_a, pitch), _words(raw_b, pitch)
    except ValueError as e:
        return {'status': 'invalid-readback', 'reason': str(e)}
    per = {}
    first = []
    max_ulp = 0
    for lane in lanes:
        c = {'bitdiff': 0, 'signed_zero': 0, 'one_sided_nan': 0, 'paired_nan': 0, 'nan_payload_diff': 0}
        side = {s: {'finite': 0, 'nan': 0, 'inf': 0, 'min': None, 'max': None} for s in 'ab'}
        for y in range(RT_H):
            for x in range(RT_W):
                a, b = wa[y][x * 4 + lane], wb[y][x * 4 + lane]
                for s, w in (('a', a), ('b', b)):
                    d = side[s]
                    if _is_nan(w):
                        d['nan'] += 1
                    elif _is_inf(w):
                        d['inf'] += 1
                    else:
                        d['finite'] += 1
                        v = _f(w)
                        d['min'] = v if d['min'] is None else min(d['min'], v)
                        d['max'] = v if d['max'] is None else max(d['max'], v)
                na, nb = _is_nan(a), _is_nan(b)
                if na and nb:
                    c['paired_nan'] += 1
                    if a != b:
                        c['nan_payload_diff'] += 1
                    continue
                if a == b:
                    continue
                c['bitdiff'] += 1
                if na or nb:
                    c['one_sided_nan'] += 1
                elif (a & 0x7FFFFFFF) == 0 and (b & 0x7FFFFFFF) == 0:
                    c['signed_zero'] += 1
                elif not (_is_inf(a) or _is_inf(b)):
                    max_ulp = max(max_ulp, abs(_ordered(a) - _ordered(b)))
                if len(first) < first_n:
                    first.append({'x': x, 'y': y, 'lane': LANES[lane], 'a': '%08x' % a, 'b': '%08x' % b})
        per[LANES[lane]] = dict(c, a=side['a'], b=side['b'])
    diff = sum(p['bitdiff'] for p in per.values())
    paired = sum(p['paired_nan'] for p in per.values())
    status = 'mismatch' if diff else ('paired-nan' if paired else 'identical')
    return {'status': status, 'bitdiff': diff, 'paired_nan': paired, 'max_ulp_finite': max_ulp,
            'first': first, 'lanes': per}


# ---- self-test: synthetic readbacks, no boot needed -------------------------
def _image(fill, pitch=RT_W * PIXEL_BYTES, edits=()):
    """fill(x, y, lane) -> u32 word; edits = [(x, y, lane, word)]."""
    buf = bytearray(pitch * RT_H)
    for y in range(RT_H):
        for x in range(RT_W):
            for lane in range(4):
                struct.pack_into('>I', buf, y * pitch + (x * 4 + lane) * 4, fill(x, y, lane))
    for x, y, lane, w in edits:
        struct.pack_into('>I', buf, y * pitch + (x * 4 + lane) * 4, w)
    return bytes(buf)


def self_test():
    base = lambda x, y, lane: struct.unpack('>I', struct.pack('>f', 0.25 + x / 128.0 - y / 256.0 + lane))[0]
    A = _image(base)
    NAN_Q, NAN_Q2 = 0x7FC00000, 0x7FC00001
    cases = [
        ('identical images', A, A, {'status': 'identical', 'bitdiff': 0}),
        ('one lane, one pixel, 1 ULP', A, _image(base, edits=[(5, 7, 2, base(5, 7, 2) + 1)]),
         {'status': 'mismatch', 'bitdiff': 1, 'max_ulp_finite': 1, 'first0': (5, 7, 'z')}),
        ('difference in the last pixel only', A, _image(base, edits=[(63, 63, 3, base(63, 63, 3) ^ 0x10)]),
         {'status': 'mismatch', 'bitdiff': 1, 'first0': (63, 63, 'w')}),
        ('+0 vs -0', _image(base, edits=[(1, 1, 0, 0x00000000)]), _image(base, edits=[(1, 1, 0, 0x80000000)]),
         {'status': 'mismatch', 'bitdiff': 1, 'signed_zero_x': 1, 'max_ulp_finite': 0}),
        ('+Inf vs -Inf', _image(base, edits=[(2, 3, 1, 0x7F800000)]), _image(base, edits=[(2, 3, 1, 0xFF800000)]),
         {'status': 'mismatch', 'bitdiff': 1, 'max_ulp_finite': 0}),
        ('NaN vs finite (one-sided)', _image(base, edits=[(4, 4, 0, NAN_Q)]), A,
         {'status': 'mismatch', 'bitdiff': 1, 'one_sided_nan_x': 1}),
        ('paired NaN, same payload -> non-PASS', _image(base, edits=[(4, 4, 0, NAN_Q)]),
         _image(base, edits=[(4, 4, 0, NAN_Q)]), {'status': 'paired-nan', 'bitdiff': 0, 'paired_nan': 1}),
        ('paired NaN, different payload', _image(base, edits=[(4, 4, 0, NAN_Q)]),
         _image(base, edits=[(4, 4, 0, NAN_Q2)]), {'status': 'paired-nan', 'bitdiff': 0, 'payload_x': 1}),
        ('truncated readback', A[:-16], A, {'status': 'invalid-readback'}),
        ('empty readback', b'', A, {'status': 'invalid-readback'}),
        ('pitch larger than a row', _image(base, pitch=1088), _image(base, pitch=1088), {'status': 'identical'}),
    ]
    fails = 0
    for name, a, b, want in cases:
        pitch = 1088 if 'pitch' in name else RT_W * PIXEL_BYTES
        got = judge(a, b, pitch=pitch)
        ok = got['status'] == want['status']
        for k in ('bitdiff', 'max_ulp_finite', 'paired_nan'):
            if k in want:
                ok &= got.get(k) == want[k]
        if 'first0' in want:
            f = got['first'][0]
            ok &= (f['x'], f['y'], f['lane']) == want['first0']
        if 'signed_zero_x' in want:
            ok &= got['lanes']['x']['signed_zero'] == want['signed_zero_x']
        if 'one_sided_nan_x' in want:
            ok &= got['lanes']['x']['one_sided_nan'] == want['one_sided_nan_x']
        if 'payload_x' in want:
            ok &= got['lanes']['x']['nan_payload_diff'] == want['payload_x']
        fails += not ok
        print('  %-44s %s  (%s)' % (name, 'ok' if ok else 'FAIL', got['status']))
    # finite min/max exclude non-finites; counts per side
    g = judge(_image(base, edits=[(0, 0, 0, NAN_Q), (1, 0, 0, 0x7F800000)]), A)
    ok = (g['lanes']['x']['a']['nan'] == 1 and g['lanes']['x']['a']['inf'] == 1 and
          g['lanes']['x']['a']['finite'] == RT_W * RT_H - 2 and math.isfinite(g['lanes']['x']['a']['max']))
    fails += not ok
    print('  %-44s %s' % ('finite min/max exclude NaN/Inf', 'ok' if ok else 'FAIL'))
    print('f32_compare self-test: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return fails == 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        sys.exit(0 if self_test() else 1)
    args = sys.argv[1:]
    pitch = RT_W * PIXEL_BYTES
    lanes = (0, 1, 2, 3)
    if '--pitch' in args:
        i = args.index('--pitch'); pitch = int(args[i + 1]); del args[i:i + 2]
    if '--lanes' in args:
        i = args.index('--lanes'); lanes = tuple(LANES.index(c) for c in args[i + 1]); del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    res = judge(open(args[0], 'rb').read(), open(args[1], 'rb').read(), pitch=pitch, lanes=lanes)
    print(json.dumps(res, indent=1))
    sys.exit(0 if res['status'] == 'identical' else 1)
