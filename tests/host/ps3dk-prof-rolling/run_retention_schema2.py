#!/usr/bin/env python3
"""Independent schema2/schema3 cold-framing controls; parent owns compiler scheduling.

The legacy 33 fixtures stay byte-identical. New Python family bodies are opaque
framing sentinels, deliberately not semantic captures. A separate ignored Rust
integration test supplies an ordinary-valid, semantically joined real container.
"""
import argparse
import json
from pathlib import Path
import shlex
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
import run_retention_inspect as base

HERE = Path(__file__).resolve().parent


def u16(b, at): return struct.unpack_from('<H', b, at)[0]
def u32(b, at): return struct.unpack_from('<I', b, at)[0]
def w16(b, at, n): struct.pack_into('<H', b, at, n)


def wrappers(flags):
    # Cold inspection checks outer wrapper framing, never inner typed state.
    a, f = bytearray(), bytearray()
    if flags & 128:
        inner = b'opaque AUTO state'
        a = bytearray(32); base.w32(a, 0, len(inner)); a += inner
    if flags & 64:
        inner = b'opaque FIFO state'
        f = bytearray(64); base.w32(f, 0, len(inner)); f += inner
    return [(int(bool(flags & 128)), bytes(a)), (int(bool(flags & 64)), bytes(f))]


def mix2(head, chunks, cut):
    flags = u32(head, 12)
    metadata = b''.join(struct.pack('<I', len(raw)) + raw for raw in chunks[:cut])
    bodies = [(cut, metadata), (0, b''), (0, b''), (0, b'')] + wrappers(flags)
    sections = b''.join(base.section(i, n, body) for i, (n, body) in enumerate(bodies, 1))
    out = bytearray(96); out[:8] = b'PFRMIX01'; struct.pack_into('<HHI', out, 8, 2, 96, 6)
    out[16:32] = head; base.w64(out, 32, cut); base.w64(out, 40, sum(map(len, chunks[:cut])))
    base.w64(out, 48, len(sections)); base.w32(out, 56, 3 | (4 if flags & 128 else 0) | (8 if flags & 64 else 0))
    struct.pack_into('<6H', out, 64, *([1] * 6)); out += sections
    out += struct.pack('<I', base.crc(out)); return bytes(out)


CENSUS = 'count max_id last_frame end reserved'.split()


def mix3(head, chunks, cut, floor=None, census=None):
    # Schema 3 = schema-2 sections behind a 128-byte header: u64 floor at 80,
    # then the retired-window census u64 count/max_id/last_frame/end/reserved.
    v2 = mix2(head, chunks, cut); out = bytearray(v2[:96]) + bytearray(32)
    w16(out, 8, 3); w16(out, 10, 128)
    base.w64(out, 80, cut if floor is None else floor)
    struct.pack_into('<5Q', out, 88, *[(census or {}).get(k, 0) for k in CENSUS])
    out += v2[96:-4]; out += struct.pack('<I', base.crc(out)); return bytes(out)


def schema3_violations(cp):
    """Independent restatement of the schema-3 framing rules for one checkpoint."""
    flags, next_ = u32(cp, 28), struct.unpack_from('<Q', cp, 32)[0]
    floor = struct.unpack_from('<Q', cp, 80)[0]
    w = dict(zip(CENSUS, struct.unpack_from('<5Q', cp, 88)))
    bad = set()
    if not flags & 128: bad.add('missing_auto128')
    if floor > next_: bad.add('floor_above_next')
    if w['max_id'] > 0xffffffff: bad.add('max_id_over_u32')
    if w['last_frame'] >= 1 << 63: bad.add('last_frame_sign')
    if w['reserved']: bad.add('census_reserved')
    if w['count'] == 0:
        if w['max_id'] or w['last_frame'] or w['end']: bad.add('partial_zero_census')
    else:
        if w['end'] >= floor: bad.add('end_not_below_floor')
        if w['count'] > w['max_id']: bad.add('count_above_max_id')
        if 2 * w['count'] > w['end'] - 1: bad.add('census_end_extent')
        if not flags & 512: bad.add('census_without_windows_flag')
    return bad


def fixture3(flags=711, floor=None, census=None):
    """Every checkpoint is schema 3; only the last carries the edited floor/census."""
    head, chunks, parts = fixture(flags)
    q = [(a, z, mix3(head, chunks, a, *((floor, census) if i == len(parts) - 1 else ())), pay)
         for i, (a, z, _, pay) in enumerate(parts)]
    return chunks, q


def fixture(flags=193, zero=False, metadata_types=None, payload_types=None):
    head, originals = base.original(2); head = bytearray(head); base.w32(head, 12, flags); head = bytes(head)
    pairs = [(u32(raw, 0), raw[16:-4]) for raw in originals[:-1]]
    if flags & 2: pairs.insert(2, (17, b'opaque PROFILE body'))
    if metadata_types is None:
        metadata_types = ([31, 33] if flags & 64 else []) + ([34, 36, 37, 38] if flags & 128 else [])
        if flags & 512: metadata_types += [41, 42]
    pairs += [(kind, b'opaque family support') for kind in metadata_types]
    boundary = len(pairs)
    if payload_types is None:
        payload_types = ([32, 43] if flags & 64 else []) + ([35] if flags & 128 else [])
    pairs += [(kind, b'opaque family payload') for kind in payload_types]
    pairs.append((6, originals[-1][16:-4]))
    chunks = [base.chunk(kind, i, body) for i, (kind, body) in enumerate(pairs)]
    cuts = [0, boundary, len(chunks)] if zero else [0, 2, boundary, len(chunks)]
    parts = [(a, z, mix2(head, chunks, a), b''.join(chunks[a:z])) for a, z in zip(cuts, cuts[1:])]
    return head, chunks, parts


def sections(cp):
    result = []; at = 96
    for _ in range(6):
        length = u32(cp, at + 8); result.append((at, at + 16, length)); at += 16 + length
    assert at == len(cp) - 4
    return result


def replace_section(cp, ordinal, count, body):
    out = bytearray(cp[:96]); locs = sections(cp)
    for i, (at, start, length) in enumerate(locs, 1):
        out += base.section(i, count, body) if i == ordinal else cp[at:start + length]
    base.w64(out, 48, len(out) - 96); out += struct.pack('<I', base.crc(out)); return bytes(out)


def controls():
    cases = list(base.controls())
    def add(name, parts, expected=0, flags=193, **options):
        first = parts[max(0, len(parts) - 2)][0] if parts else 0
        if 'schema3' in options: options['checkpoints'] = [cp for _, _, cp, _ in parts[-2:]]
        cases.append(dict(name=('' if name.startswith('schema3_') else 'schema2_') + name, blob=base.container(parts), expected=expected,
                          kind=2, flags=flags, first=first, next=parts[-1][1] if parts else 0,
                          generation=len(parts) + 2, framing_only=True, **options))
    for flags in [65, 129, 193, 711]:
        for zero in [False, True]:
            _, _, parts = fixture(flags, zero)
            add(f'flags{flags}_' + ('chunk0' if zero else 'evicted_header'), parts, flags=flags)
    _, chunks, parts = fixture()
    def mutate(name, edit, expected=-3, source=parts):
        q = list(source); cp = bytearray(q[-1][2]); result = edit(cp)
        cp = base.repair(cp) if result is None else result
        q[-1] = (*q[-1][:2], cp, q[-1][3]); add(name, q, expected)
    for name, at, width, value in [
        ('fixed_header', 10, 2, 80), ('section_count', 12, 4, 4),
        ('section_extent', 48, 8, 1), ('family_mask', 56, 4, 3),
        ('reserved_word', 60, 4, 1), ('reserved_tail', 95, 1, 1),
        ('unknown_schema', 8, 2, 4), ('unknown_family_schema', 72, 2, 2),
        ('missing_family_flags', 28, 4, 1), ('mixed_original_flags', 28, 4, 705), ('unknown_original_flag', 28, 4, 1217),
    ]:
        def edit(cp, at=at, width=width, value=value): cp[at:at + width] = value.to_bytes(width, 'little')
        mutate(name, edit, -6 if name.startswith('unknown_') else -3)
    mutate('old_schema_with_new_flags', lambda cp: base.checkpoint(2, bytes(cp[16:32]), chunks, parts[-1][0]))
    for ordinal in [5, 6]:
        mutate(f'section{ordinal}_missing_presence', lambda cp, i=ordinal: replace_section(cp, i, 0, cp[sections(cp)[i-1][1]:sections(cp)[i-1][1]+sections(cp)[i-1][2]]))
        mutate(f'section{ordinal}_short_wrapper', lambda cp, i=ordinal: replace_section(cp, i, 1, bytes(31 if i == 5 else 63)))
        def length_bad(cp, i=ordinal): base.w32(cp, sections(cp)[i-1][1], 0xffffffff)
        mutate(f'section{ordinal}_inner_extent', length_bad)
        def reserved_bad(cp, i=ordinal): cp[sections(cp)[i-1][1]+(16 if i == 5 else 56)] = 1
        mutate(f'section{ordinal}_reserved', reserved_bad)
    def groups_bad(cp): base.w32(cp, sections(cp)[5][1]+4, 1)
    mutate('fifo_group_extent', groups_bad)
    def order_bad(cp): w16(cp, sections(cp)[4][0], 6)
    mutate('section_order', order_bad)
    def section_schema(cp): w16(cp, sections(cp)[4][0]+2, 2)
    mutate('section_schema_unknown', section_schema, -6)
    for flags, ordinal in [(65, 5), (129, 6)]:
        _, _, q = fixture(flags)
        cp = replace_section(q[-1][2], ordinal, 1, wrappers(193)[ordinal-5][1])
        q[-1] = (*q[-1][:2], cp, q[-1][3]); add(f'unadvertised_section{ordinal}', q, -3, flags=flags)
    for kind in [32, 35, 43]:
        _, _, q = fixture(metadata_types=[kind], payload_types=[]); add(f'event{kind}_in_metadata', q, -3)
    for kind in [31, 32, 33, 43, 34, 35, 36, 37, 38]:
        flags = 129 if kind in [31, 32, 33, 43] else 65
        _, _, q = fixture(flags, payload_types=[kind]); add(f'unadvertised_payload{kind}', q, -6, flags=flags)
    for kind in [31, 33, 34, 36, 37, 38]:
        flags = 129 if kind in [31, 33] else 65
        _, _, q = fixture(flags, metadata_types=[kind], payload_types=[]); add(f'unadvertised_metadata{kind}', q, -6, flags=flags)
    add('wrong_app', parts, -3, app='application'); add('wrong_session', parts, -3, session=base.SESSION+1)
    add('short_reads', parts, mode='short')
    good = base.container(parts)
    for name, state, reason in [('open', 1, 0), ('resource_stopped', 3, 1)]:
        cases.append(dict(name='schema2_' + name, blob=base.container(parts, state=state, reason=reason), expected=-5))
    q = list(parts); q[-1] = (q[-1][0], q[-1][1]-1, q[-1][2], q[-1][3][:-len(chunks[-1])]); add('missing_trailer', q, -5)
    q = list(parts); cp = bytearray(q[-1][2]); base.w64(cp, 32, len(chunks)); base.w64(cp, 40, sum(map(len, chunks)))
    q.append((len(chunks), len(chunks), base.repair(cp), b'')); add('trailer_then_empty_segment', q, -3)
    _, profile_chunks, q = fixture(711)
    profile_chunks[2], profile_chunks[3] = profile_chunks[3], profile_chunks[2]
    profile_chunks = [base.chunk(u32(raw, 0), i, raw[16:-4]) for i, raw in enumerate(profile_chunks)]
    head = q[0][2][16:32]
    q = [(a, z, mix2(head, profile_chunks, a), b''.join(profile_chunks[a:z])) for a, z, _, _ in q]
    add('profile_placement', q, -3, flags=711)
    # Schema 3: retired-window census framing. Each negative breaks exactly
    # one rule of a valid envelope (asserted by schema3_violations below).
    retired = dict(count=3, max_id=5, last_frame=1000, end=7)
    for name, flags, floor, census in [
        ('schema3_no_retired_windows', 193, None, None), ('schema3_no_retired_windows_flags711', 711, None, None),
        ('schema3_retired_windows', 711, None, retired),
        ('schema3_retired_windows_tight', 711, 8, dict(count=3, max_id=3, last_frame=(1 << 63) - 1, end=7)),
    ]:
        _, q = fixture3(flags, floor, census); add(name, q, flags=flags, schema3=set())
    for name, flags, floor, census in [
        ('floor_above_next', 711, 13, retired),
        ('max_id_over_u32', 711, None, retired | dict(max_id=1 << 32)),
        ('last_frame_sign', 711, None, retired | dict(last_frame=1 << 63)),
        ('end_not_below_floor', 711, 7, retired),
        ('census_reserved', 711, None, retired | dict(reserved=1)),
        ('partial_zero_census', 711, None, dict(max_id=5)),
        ('partial_zero_census_last_frame', 711, None, dict(last_frame=7)),
        ('partial_zero_census_end', 711, None, dict(end=7)),
        ('count_above_max_id', 711, None, retired | dict(max_id=2)),
        ('census_end_extent', 711, None, retired | dict(end=6)),
        ('census_without_windows_flag', 193, None, retired),
        ('missing_auto128', 65, None, None),
    ]:
        _, q = fixture3(flags, floor, census)
        rule = 'partial_zero_census' if name.startswith('partial_zero_census') else name
        add('schema3_' + name, q, -3, flags=flags, schema3={rule})
    _, q = fixture3(); cp = bytearray(q[-1][2]); w16(cp, 8, 4)
    q[-1] = (*q[-1][:2], base.repair(cp), q[-1][3]); add('schema3_unknown_version4', q, -6, flags=711)
    # Checkpoint corruption cannot be hidden by valid outer slot/root CRCs.
    mutate('checkpoint_crc', lambda cp: bytes(cp[:-1]) + bytes([cp[-1] ^ 1]))
    for name, changes in [('checkpoint_cap', dict(max_cp=1)), ('metadata_cap', dict(max_meta=16)),
                          ('file_cap', dict(max_file=base.FILE-1)), ('slot_cap', dict(max_slot=base.SLOT-1))]:
        add(name, parts, -4, **changes)
    altered = bytearray(good); altered[base.PREFIX+96] ^= 1
    cases.append(dict(name='schema2_digest_old_slot', blob=bytes(altered), expected=0, kind=2, flags=193,
                      first=2, next=len(chunks), generation=5, framing_only=True))
    for c in cases:
        if 'schema3' in c:
            got = set().union(*(schema3_violations(cp) for cp in c.pop('checkpoints')))
            assert got == c['schema3'], (c['name'], got)
        c.pop('checkpoints', None)
    assert len({c['name'] for c in cases}) == len(cases)
    assert all(len(c['blob']) == base.FILE for c in cases)
    return cases


def passes(case, got):
    if got.get('rc') != case['expected'] or got.get('writes') != 0: return False
    if got['rc']: return got.get('unchanged') == 1
    if case.get('op') == 'measure':
        return got.get('file') == base.FILE and got.get('slot') == base.SLOT and got.get('scratch') == base.SLOT and got.get('store', 0) > 0
    kind, first = case.get('kind', 1), case.get('first', 2)
    expected = dict(file=base.FILE, crc=base.crc(case['blob']), evidence=1, kind=kind,
                    flags=case.get('flags', 0 if kind == 1 else 1), first=first, next=case.get('next', 4),
                    generation=case.get('generation', 4 if first == 0 else 5), recovered=0)
    return all(got.get(key) == value for key, value in expected.items())


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--build-dir', type=Path); p.add_argument('--cc', default='cc')
    p.add_argument('--check-only', action='store_true'); a = p.parse_args(); cases = controls()
    if a.check_only:
        print(f'CHECK_ONLY {len(cases)} framing controls; no build, execution or files'); return 0
    if a.build_dir is None: p.error('--build-dir is required without --check-only')
    out = a.build_dir.resolve(); out.mkdir(parents=True, exist_ok=False); exe = out/'retention-inspect'
    command = shlex.split(a.cc)+['-std=c11','-O1','-Wall','-Wextra','-Werror','-pedantic','-I',str(base.IMPL),
        str(HERE/'retention_inspect_test.c'),str(base.IMPL/'roll_retention_inspect.c'),str(base.IMPL/'roll_store.c'),'-o',str(exe)]
    subprocess.run(command, check=True, timeout=120)
    print(f'PS3DK_PROF_RETENTION_INSPECT={exe}', flush=True)
    results = []
    for c in cases:
        path = out/(c['name']+'.profr'); path.write_bytes(c['blob'])
        def invoke(physical=False):
            defaults = dict(max_file=base.FILE,max_slot=base.SLOT,max_cp=base.CAP,max_meta=base.CAP,
                            slot_bytes=base.SLOT,work_bytes=4096,mode='normal')
            values = defaults if physical else defaults | c
            command = [str(exe), 'physical' if physical else c.get('op','inspect'), str(path), c.get('app','app'),str(c.get('session',base.SESSION))]
            command += [str(values[k]) for k in defaults]
            r = subprocess.run(command,capture_output=True,text=True,timeout=20)
            if r.returncode: raise RuntimeError((c['name'], r.returncode, r.stdout, r.stderr))
            return json.loads(r.stdout)
        physical = invoke(True)
        if (physical['rc']==0)!=c.get('physical_ok',True) or physical['writes']!=0:
            raise RuntimeError(('invalid independent physical fixture',c['name'],physical))
        got = invoke(); ok = passes(c,got)
        results.append(dict(name=c['name'],passed=ok,expected=c['expected'],observed=got,framing_only=c.get('framing_only',False)))
        print(f"{c['name']}: {'PASS' if ok else 'FAIL'} (expected {c['expected']}, got {got['rc']})",flush=True)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f"{sum(c['passed'] for c in results)}/{len(results)} cold controls passed")
    return int(not all(c['passed'] for c in results))


if __name__=='__main__': raise SystemExit(main())
