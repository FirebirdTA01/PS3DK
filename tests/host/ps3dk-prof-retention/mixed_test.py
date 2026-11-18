#!/usr/bin/env python3
"""Mixed-format completion index RED controls; caller owns build authority.

Default driver is a test-only rejecting stand-in. --implementation is reserved
for the parent-authorized shared-header implementation after observed RED.
Index bytes and completed capture fixtures are independently constructed.
Cold inspector success is not runtime semantic completion authorization.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ROLL = HERE.parent / 'ps3dk-prof-rolling'
sys.path.insert(0, str(ROLL))
import run_retention_inspect as physical

PROF, PROFR = 1, 2
SLOT, CP, FILE = physical.SLOT, physical.CAP, physical.FILE
INDEX_CAP = 64 + 1025 * 24 + 4


def original(app, session, complete=True):
    strings = [app.encode('ascii'), b'build', b'sdk', b'compiler', b'', b'mixed']
    header = b''.join(struct.pack('<H', len(s)) + s for s in strings)
    header += struct.pack('<BBHQIII', 1, 1, 0, session, 512, 2, 8)
    head = b'PS3DKPRF' + struct.pack('<II', 1, 0)
    chunks = [physical.chunk(1, 0, header),
              physical.chunk(2, 1, struct.pack('<IIIQQ', 1, 1, 0, 79800000, session)),
              physical.chunk(3, 2, physical.descriptor_payload())]
    if complete:
        chunks.append(physical.chunk(6, 3, struct.pack('<IIQiIQQ', 0, 0, 0, 0, 0, 0, 1)))
    return head, chunks


def rolling(app, session, slot=SLOT, state=2, complete=True):
    head, chunks = original(app, session, complete)
    cuts = [0, 2, 3, len(chunks)]
    parts = [(a, b, physical.checkpoint(1, head, chunks, a), b''.join(chunks[a:b]))
             for a, b in zip(cuts, cuts[1:])]
    # The independent encoder's constants describe one fixture, never production.
    old = physical.SLOT, physical.CAP, physical.FILE
    try:
        physical.SLOT = slot; physical.CAP = (slot - 4096) // 2
        physical.FILE = physical.PREFIX + 3 * slot
        return physical.container(parts, state=state, reason=1 if state == 3 else 0)
    finally:
        physical.SLOT, physical.CAP, physical.FILE = old


def index_bytes(app, entries, order, version=2):
    out = bytearray(64); out[:8] = b'P3RETAIN'
    struct.pack_into('<IIQ', out, 8, version, len(entries), order)
    encoded = app.encode('ascii'); assert 1 <= len(encoded) <= 32
    out[24:24 + len(encoded)] = encoded
    for session, kind, blob in entries:
        out += struct.pack('<QQII', session, len(blob), zlib.crc32(blob), kind if version == 2 else 0)
    return bytes(out) + struct.pack('<I', zlib.crc32(out))


def decode_index(path):
    raw = path.read_bytes()
    assert len(raw) >= 68 and raw[:8] == b'P3RETAIN', 'index framing'
    version, count, order = struct.unpack_from('<IIQ', raw, 8)
    assert version == 2, f'expected single version2 index, got {version}'
    assert len(raw) == 68 + 24 * count and struct.unpack_from('<I', raw, len(raw) - 4)[0] == zlib.crc32(raw[:-4]), 'index length/CRC'
    rows = [struct.unpack_from('<QQII', raw, 64 + i * 24) for i in range(count)]
    assert all(r[3] in [PROF, PROFR] for r in rows), 'format tag'
    assert len({r[0] for r in rows}) == len(rows), 'duplicate indexed session'
    return order, rows


def unchanged(directory):
    return {p.name: ('link', os.readlink(p)) if p.is_symlink() else ('file', p.read_bytes())
            for p in directory.iterdir() if p.is_symlink() or p.is_file()}


class Cases:
    def __init__(self, exe, inspect, output):
        self.exe, self.inspect, self.output = exe, inspect, output
        self.current = None
        self.observations = []

    def capture(self, directory, session, kind=PROF, app='app', slot=SLOT, state=2, complete=True):
        suffix = 'prof' if kind == PROF else 'profr'
        path = directory / f'{app}-{session:016x}.{suffix}'
        if kind == PROF:
            head, chunks = original(app, session, complete); blob = head + b''.join(chunks)
        else:
            blob = rolling(app, session, slot, state, complete)
        path.write_bytes(blob)
        if kind == PROFR:
            # Independently accepted physical shape, then known Unit A evidence.
            limits = [len(blob), slot, (slot - 4096) // 2, (slot - 4096) // 2, slot, 4096]
            for op in ['physical', 'inspect']:
                run = subprocess.run([str(self.inspect), op, str(path), app, str(session),
                                      *map(str, limits), 'normal'], capture_output=True, text=True, timeout=20)
                assert run.returncode == 0, ('physical preflight process', run.stderr)
                got = json.loads(run.stdout)
                expected = 0 if op == 'physical' or (state == 2 and complete) else -5
                assert got['rc'] == expected and got['writes'] == 0, ('independent fixture preflight', op, got)
        return path

    def seed(self, directory, entries, order, version=2, app='app'):
        path = directory / '.ps3dk-retain-app.index'
        path.write_bytes(index_bytes(app, [(s, k, p.read_bytes()) for s, k, p in entries], order, version))
        return path

    def run(self, path, session, keep=2, kind=PROF, app='app', expected=0,
            op='kind', max_file=FILE, max_slot=SLOT, max_cp=CP, max_meta=CP, fault=None):
        env = dict(os.environ)
        env.pop('PR_MIX_FAULT', None)
        if fault: env['PR_MIX_FAULT'] = fault
        command = [str(self.exe), op, str(path), app, f'{session:x}', str(keep), str(kind),
                   str(max_file), str(max_slot), str(max_cp), str(max_meta)]
        run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
        assert run.returncode == 0, ('driver process failure, not behavioral RED', run.returncode, run.stderr)
        got = json.loads(run.stdout)
        self.observations.append(dict(command=command[1:], result=got, trace=run.stderr))
        assert got['rc'] == expected, f"expected retention={expected}, got {got['rc']}"
        assert got['lock_violations'] == 0, 'index operation outside per-app lock'
        assert got['early_prune'] == 0, 'capture deletion attempted before index publication'
        if fault: assert got['fault_hit'] == 1, f'fault did not reach named stage: {fault}'
        return got

    def expect_index(self, path, order, identities):
        seen_order, rows = decode_index(path)
        assert seen_order == order, ('completion ordinal changed', seen_order, order)
        assert [(r[0], r[3]) for r in rows] == identities, ('one cross-format completion order', rows, identities)
        for session, size, digest, kind in rows:
            file = path.parent / f'app-{session:016x}.{"prof" if kind == PROF else "profr"}'
            if file.exists():
                assert len(file.read_bytes()) == size and zlib.crc32(file.read_bytes()) == digest, 'enrolled digest differs'


def controls(c):
    cases = []
    def add(name, function): cases.append((name, function))

    def alternating(d):
        unrelated = c.capture(d, 99, PROFR, app='application')
        untracked = c.capture(d, 10); stopped = c.capture(d, 11, PROFR, state=3)
        paths = [(s, k, c.capture(d, s, k)) for s, k in [(8, PROF), (3, PROFR), (7, PROF), (2, PROFR)]]
        for i, (s, k, p) in enumerate(paths):
            os.utime(p, (1000 + (4 - i) * 100, 1000 + (4 - i) * 100))
            c.run(p, s, kind=k)
        assert not paths[0][2].exists() and not paths[1][2].exists(), 'oldest across formats not evicted'
        assert paths[2][2].exists() and paths[3][2].exists()
        assert unrelated.exists() and untracked.exists() and stopped.exists(), 'unowned capture removed'
        c.expect_index(d / '.ps3dk-retain-app.index', 4, [(7, PROF), (2, PROFR)])
        c.run(unrelated, 99, keep=1, kind=PROFR, app='application')
        assert unrelated.exists() and (d / '.ps3dk-retain-application.index').exists()
    add('one_completion_order_across_prof_and_profr', alternating)

    def migration(d):
        a, b, new = c.capture(d, 9), c.capture(d, 4), c.capture(d, 2, PROFR)
        ledger = c.seed(d, [(9, PROF, a), (4, PROF, b)], 19, version=1)
        c.run(new, 2, kind=PROFR)
        c.expect_index(ledger, 20, [(4, PROF), (2, PROFR)])
        assert not a.exists() and b.exists() and new.exists()
        assert sorted(p.name for p in d.glob('*.index')) == ['.ps3dk-retain-app.index']
    add('v1_migration_preserves_order_and_ordinal_holes', migration)

    def duplicate(d):
        old, new = c.capture(d, 8), c.capture(d, 8, PROFR)
        c.seed(d, [(8, PROF, old)], 1); before = unchanged(d)
        c.run(new, 8, kind=PROFR, expected=-1)
        assert unchanged(d) == before, 'cross-format duplicate changed order/files'
    add('same_session_other_format_cannot_refresh_completion', duplicate)

    invalid_indexes = ['cross_tag_duplicate', 'tag_zero', 'tag_unknown', 'v1_reserved_tag',
                       'wrong_app', 'bad_crc', 'count_length', 'ordinal_below_count', 'ordinal_overflow', 'unknown_version']
    for mode in invalid_indexes:
        def invalid(d, mode=mode):
            old, other, new = c.capture(d, 8), c.capture(d, 8, PROFR), c.capture(d, 3, PROFR)
            entries = [(8, PROF, old)]
            if mode == 'cross_tag_duplicate': entries.append((8, PROFR, other))
            ledger = c.seed(d, entries, max(2, len(entries)), version=1 if mode == 'v1_reserved_tag' else 2,
                            app='application' if mode == 'wrong_app' else 'app')
            raw = bytearray(ledger.read_bytes())
            edits = {'tag_zero': (84, '<I', 0), 'tag_unknown': (84, '<I', 3), 'v1_reserved_tag': (84, '<I', 1),
                     'count_length': (12, '<I', 5), 'ordinal_below_count': (16, '<Q', 0),
                     'ordinal_overflow': (16, '<Q', (1 << 64) - 1), 'unknown_version': (8, '<I', 3)}
            if mode in edits:
                at, fmt, value = edits[mode]; struct.pack_into(fmt, raw, at, value)
                struct.pack_into('<I', raw, len(raw) - 4, zlib.crc32(raw[:-4]))
            if mode == 'bad_crc': raw[-1] ^= 1
            ledger.write_bytes(raw); before = unchanged(d)
            c.run(new, 3, kind=PROFR, expected=-1)
            assert unchanged(d) == before, 'malformed index changed before refusal'
        add('reject_index_' + mode, invalid)

    for mode in ['keep_zero', 'keep_large', 'format_zero', 'format_unknown', 'wrong_basename', 'wrong_suffix']:
        def invalid_argument(d, mode=mode):
            old, new = c.capture(d, 8), c.capture(d, 3, PROFR)
            c.seed(d, [(8, PROF, old)], 1)
            path, keep, kind = new, 1, PROFR
            if mode == 'keep_zero': keep = 0
            if mode == 'keep_large': keep = 1025
            if mode == 'format_zero': kind = 0
            if mode == 'format_unknown': kind = 3
            if mode == 'wrong_basename':
                path = d / 'arbitrary.profr'; path.write_bytes(new.read_bytes())
            if mode == 'wrong_suffix': path = c.capture(d, 3, PROF)
            before = unchanged(d)
            c.run(path, 3, keep=keep, kind=kind, expected=-1)
            assert unchanged(d) == before, 'invalid path/options modified retention'
        add('reject_' + mode, invalid_argument)

    for operation in ['legacy', 'kind-no-budget']:
        def no_budget(d, operation=operation):
            old, new = c.capture(d, 8, PROFR), c.capture(d, 3)
            c.seed(d, [(8, PROFR, old)], 1)
            c.run(new, 3, keep=1, expected=-1, op=operation)
            assert old.exists() and new.exists(), 'no-budget call deleted rolling entry'
        add('rolling_old_entry_without_budget_' + operation, no_budget)

    def current_no_budget(d):
        new = c.capture(d, 3, PROFR); before = unchanged(d)
        c.run(new, 3, kind=PROFR, expected=-1, op='kind-no-budget')
        assert unchanged(d) == before
    add('rolling_current_without_budget', current_no_budget)

    def resource(d):
        old, new = c.capture(d, 8, PROFR, slot=32768), c.capture(d, 3)
        ledger = c.seed(d, [(8, PROFR, old)], 1)
        got = c.run(new, 3, keep=1, expected=-1, max_file=physical.PREFIX + 3 * 32768)
        assert old.exists() and new.exists(), 'resource refusal deleted protected files'
        assert got['largest_allocation'] <= INDEX_CAP, 'allocated old full slot before checking caller limits'
        c.expect_index(ledger, 2, [(8, PROFR), (3, PROF)])
        newest = c.capture(d, 2)
        c.run(newest, 2, keep=1, max_file=physical.PREFIX + 3 * 32768, max_slot=32768, max_cp=14336, max_meta=14336)
        c.expect_index(ledger, 3, [(2, PROF)])
        assert newest.exists() and not old.exists() and not new.exists(), 'larger budget did not retry pending eviction'
    add('older_larger_slot_refuses_without_allocation_then_retry', resource)

    def old_digest(d):
        old, new = c.capture(d, 8, PROFR), c.capture(d, 3)
        ledger = c.seed(d, [(8, PROFR, old)], 1)
        changed = bytearray(old.read_bytes()); changed[physical.PREFIX + 96] ^= 1; old.write_bytes(changed)
        c.run(new, 3, keep=1, expected=-1)
        assert old.read_bytes() == bytes(changed) and new.exists(), 'changed indexed capture deleted'
        order, rows = decode_index(ledger)
        assert order == 2 and [(r[0], r[3]) for r in rows] == [(8, PROFR), (3, PROF)]
    add('whole_file_digest_mismatch_preserves_old_capture', old_digest)

    for mode in ['open_current', 'stopped_current', 'missing_trailer', 'open_old']:
        def incomplete(d, mode=mode):
            old = c.capture(d, 8, PROFR, state=1 if mode == 'open_old' else 2)
            ledger = c.seed(d, [(8, PROFR, old)], 1)
            new = c.capture(d, 3, PROFR, state=1 if mode == 'open_current' else 3 if mode == 'stopped_current' else 2,
                            complete=mode != 'missing_trailer')
            prior = ledger.read_bytes(); c.run(new, 3, keep=1, kind=PROFR, expected=-1)
            assert old.exists() and new.exists(), 'incomplete capture removed'
            if mode != 'open_old': assert ledger.read_bytes() == prior, 'incomplete current enrolled'
        add('refuse_' + mode, incomplete)

    def old_symlink(d):
        old, new = c.capture(d, 8, PROFR), c.capture(d, 3)
        c.seed(d, [(8, PROFR, old)], 1)
        saved = d / 'foreign-bytes'; old.rename(saved); old.symlink_to(saved.name)
        c.run(new, 3, keep=1, expected=-1)
        assert old.is_symlink() and saved.exists() and new.exists()
    add('indexed_rolling_symlink_is_not_deleted', old_symlink)

    def missing_old(d):
        old, new = c.capture(d, 8, PROFR), c.capture(d, 3)
        ledger = c.seed(d, [(8, PROFR, old)], 1); old.unlink()
        c.run(new, 3, keep=1); c.expect_index(ledger, 2, [(3, PROF)])
    add('missing_indexed_path_retries_post_unlink_save', missing_old)

    for extension in ['lock', 'tmp']:
        def foreign(d, extension=extension):
            old, new = c.capture(d, 8), c.capture(d, 3, PROFR)
            c.seed(d, [(8, PROF, old)], 1)
            marker = d / ('.ps3dk-retain-app.' + extension); marker.write_bytes(b'foreign holder')
            before = unchanged(d); c.run(new, 3, keep=1, kind=PROFR, expected=-1)
            assert unchanged(d) == before, 'removed or rewrote foreign lock/temp'
        add('foreign_' + extension + '_preserved', foreign)

    faults = [('sync-capture:1', 'before'), ('close-capture:1', 'before'), ('close-index:1', 'before'),
              ('write-temp:1', 'before'), ('sync-temp:1', 'before'), ('close-temp:1', 'before'),
              ('rename-index:1', 'before'), ('unlink-capture:1', 'appended'),
              ('write-temp:2', 'removed'), ('sync-temp:2', 'removed'), ('close-temp:2', 'removed'),
              ('rename-index:2', 'removed'), ('close-lock:1', 'completed'), ('unlink-lock:1', 'completed')]
    for fault, stage in faults:
        def failure(d, fault=fault, stage=stage):
            a, b, new = c.capture(d, 9), c.capture(d, 4), c.capture(d, 2, PROFR)
            ledger = c.seed(d, [(9, PROF, a), (4, PROF, b)], 19, version=1)
            prior = ledger.read_bytes(); c.run(new, 2, kind=PROFR, expected=-1, fault=fault)
            assert b.exists() and new.exists(), 'fault removed protected recent/current capture'
            if stage == 'before':
                assert ledger.read_bytes() == prior and a.exists(), 'migration published/pruned after failed prerequisite'
            elif stage == 'appended':
                assert a.exists(); c.expect_index(ledger, 20, [(9, PROF), (4, PROF), (2, PROFR)])
            elif stage == 'removed':
                assert not a.exists(); c.expect_index(ledger, 20, [(9, PROF), (4, PROF), (2, PROFR)])
                newer = c.capture(d, 7)
                c.run(newer, 7)
                c.expect_index(ledger, 21, [(2, PROFR), (7, PROF)])
                assert new.exists() and newer.exists() and not b.exists(), 'failed prune-save did not retry safely'
            else:
                assert not a.exists(); c.expect_index(ledger, 20, [(4, PROF), (2, PROFR)])
        add('fault_' + fault.replace(':', '_'), failure)
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--implementation', action='store_true', help='use authorized real shared header after observed RED')
    parser.add_argument('--check-only', action='store_true', help='construct bytes and list controls, no files/compiler')
    args = parser.parse_args()
    if args.check_only:
        assert len(rolling('app', 7)) == FILE
        assert len(rolling('app', 7, slot=32768)) == physical.PREFIX + 3 * 32768
        cases = controls(None)
        assert len({name for name, _ in cases}) == len(cases)
        for name, _ in cases: print(name)
        print(f'CHECK_ONLY {len(cases)} controls; no files/builds/execution')
        return 0
    if args.build_dir is None: parser.error('--build-dir required')
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents: parser.error('use an external build directory')
    out.mkdir(parents=True, exist_ok=False)
    impl = ROOT / 'sdk/libps3dkprof/src/rolling'
    common = shlex.split(args.cc) + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(impl)]
    exe, inspect = out / 'mixed-retention', out / 'physical-inspection'
    command = common + ['-I', str(ROOT / 'sdk/libps3dkprof/src/pal'), str(HERE / 'mixed_driver.c')]
    if args.implementation:
        command += ['-DMIXED_RETENTION_IMPLEMENTED', str(impl / 'roll_store.c'),
                    str(impl / 'roll_retention_inspect.c'), str(impl / 'roll_file.c')]
    subprocess.run(command + ['-o', str(exe)], check=True, timeout=120)
    subprocess.run(common + [str(ROLL / 'retention_inspect_test.c'), str(impl / 'roll_retention_inspect.c'),
                            str(impl / 'roll_store.c'), '-o', str(inspect)], check=True, timeout=120)
    c = Cases(exe, inspect, out); results = []
    for name, function in controls(c):
        directory = out / name; directory.mkdir(); c.observations = []
        try:
            function(directory); error = None
        except (AssertionError, ValueError, subprocess.TimeoutExpired) as failure:
            error = str(failure)
        results.append(dict(name=name, passed=error is None, error=error, observations=c.observations))
        print(f"{name}: {'PASS' if error is None else 'FAIL ' + error}", flush=True)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    passed = sum(r['passed'] for r in results)
    print(f'{passed}/{len(results)} mixed-format retention controls passed')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
