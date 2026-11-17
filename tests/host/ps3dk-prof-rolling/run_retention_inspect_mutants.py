#!/usr/bin/env python3
"""Isolated cold-inspector sensitivity; caller owns all compiler authority.

An unchanged 33-control baseline and independent physical fixture checks must
pass first. Each compiled mutant must fail its named existing witness by the
specified observable defect. Compilation/process errors are harness failures.
Writes only to a fresh external directory; never edits checkout sources/tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
import run_retention_inspect as fixture

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
IMPL = ROOT / 'sdk/libps3dkprof/src/rolling'


class GateError(Exception):
    pass


def specifications():
    # Each edit is independent, starting from the unchanged source snapshot.
    return [
        ('exact_app', 'wrong_exact_app', 'accepted', [
            ('if (!i && (len != id->app_bytes || memcmp(p, id->app, len)))',
             'if (0 && !i && (len != id->app_bytes || memcmp(p, id->app, len)))')]),
        ('original_session', 'wrong_original_session', 'accepted', [
            (' || r64(p + 4) != id->session', '')]),
        ('sealed_state', 'open_is_not_completed', 'accepted', [
            ('view.state != PFR_SEALED || view.reason || !view.retained_segments',
             'view.reason || !view.retained_segments')]),
        ('actual_trailer', 'sealed_without_real_trailer', 'accepted', [
            ('!have_trailer || !id.have_identity || next <', '!id.have_identity || next <')]),
        ('terminal_before_empty', 'trailer_followed_by_empty_segment', 'accepted', [
            ('i + 1 != view.retained_segments || len != remain', 'len != remain')]),
        ('checkpoint_crc', 'checkpoint_crc_not_outer_crc', 'accepted', [
            ('if (crc_bytes(p, n - 4) != r32(p + n - 4)) return PFI_CORRUPT;',
             '/* MUTANT: trust outer slot CRC instead of checkpoint CRC. */')]),
        ('checkpoint_chunk_frontier', 'checkpoint_chunk_frontier', 'accepted', [
            (' || next != seg->first_chunk', '')]),
        ('checkpoint_byte_frontier', 'checkpoint_byte_frontier', 'accepted', [
            (' || bytes != prefix_bytes', ' || (0 && bytes != prefix_bytes)')]),
        ('full_file_digest', 'digest_covers_unreferenced_old_slot', 'digest', [
            ('crc = crc_add(crc, buffers->work, n); off += n;',
             'if (off < UINT64_C(12288) || off >= UINT64_C(20480))\n'
             '            crc = crc_add(crc, buffers->work, n);\n'
             '        off += n; /* MUTANT: skip the unreferenced old slot. */')]),
        ('file_budget', 'file_budget', 'accepted', [
            ('if (extent > limits->max_file_bytes) return PFI_SPACE;',
             '/* MUTANT: omit caller file ceiling. */')]),
        ('slot_budget', 'slot_budget', 'accepted', [
            ('r32(p + 44) > limits->max_slot_bytes\n        || ', ''),
            ('if (q.layout.slot_bytes > limits->max_slot_bytes) return PFI_SPACE;',
             '/* MUTANT: omit caller slot ceiling. */')]),
        ('checkpoint_budget', 'checkpoint_budget', 'accepted', [
            ('|| q.config.checkpoint_bytes > limits->max_checkpoint_bytes',
             '|| (0 && q.config.checkpoint_bytes > limits->max_checkpoint_bytes)'),
            ('if (n > limits->max_checkpoint_bytes) return PFI_SPACE;',
             '/* MUTANT: omit encoded checkpoint ceiling too. */')]),
        ('metadata_budget', 'metadata_budget', 'accepted', [
            ('if (limits->max_metadata_bytes < 16 || bytes > limits->max_metadata_bytes - 16u)',
             'if (0 && (limits->max_metadata_bytes < 16 || bytes > limits->max_metadata_bytes - 16u))')]),
        ('writes_disabled', 'kind1_evicted_header', 'writes', [
            ('ro = *io; ro.write_at = NULL; ro.resize = NULL; ro.sync = NULL;',
             'ro = *io;\n'
             '    if (ro.sync) (void)ro.sync(ro.ctx); /* MUTANT: sync during cold inspection. */\n'
             '    ro.write_at = NULL; ro.resize = NULL; ro.sync = NULL;')]),
    ]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def snapshot():
    files = [IMPL / 'roll_retention_inspect.c', IMPL / 'roll_retention_inspect.h',
             IMPL / 'roll_store.c', IMPL / 'roll_store.h',
             HERE / 'retention_inspect_test.c', HERE / 'run_retention_inspect.py', Path(__file__).resolve()]
    for path in files:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise GateError(f'not a bounded regular source: {path}')
    return {path: path.read_bytes() for path in files}


def variants(data, cases):
    source = data[IMPL / 'roll_retention_inspect.c'].decode('utf-8').replace('\r\n', '\n')
    by_name = {c['name']: c for c in cases}
    if len(by_name) != len(cases) or len(cases) != 33:
        raise GateError('review changed 33-control inventory before running')
    result = []
    for name, witness, defect, edits in specifications():
        if witness not in by_name:
            raise GateError(f'missing named witness: {witness}')
        changed = source
        for before, after in edits:
            if changed.count(before) != 1:
                raise GateError(f'missing/ambiguous mutation anchor: {name}: {before!r}')
            changed = changed.replace(before, after, 1)
        result.append(dict(name=name, witness=witness, defect=defect, source=changed))
    if len({v['source'] for v in result}) != len(result) or any(v['source'] == source for v in result):
        raise GateError('mutants must be distinct changes')
    return result, by_name


def compile_driver(cc, directory, shared, source):
    directory.mkdir()
    path = directory / 'roll_retention_inspect.c'
    path.write_bytes(source)
    exe = directory / 'retention-inspect'
    command = cc + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-I', str(shared), str(shared / 'retention_inspect_test.c'), str(path),
                    str(shared / 'roll_store.c'), '-o', str(exe)]
    run = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (directory / 'build.log').write_text(run.stdout + run.stderr, encoding='utf-8')
    if run.returncode:
        raise GateError(f'{directory.name}: compiler failure ({run.returncode}); not a killed mutant')
    return exe


def invoke(exe, case, files, physical=False):
    normal = dict(max_file=fixture.FILE, max_slot=fixture.SLOT, max_cp=fixture.CAP,
                  max_meta=fixture.CAP, slot_bytes=fixture.SLOT, work_bytes=4096, mode='normal')
    opts = normal if physical else normal | case
    command = [str(exe), 'physical' if physical else case.get('op', 'inspect'), str(files / (case['name'] + '.profr')),
               case.get('app', 'app'), str(case.get('session', fixture.SESSION))]
    command += [str(opts[k]) for k in ['max_file', 'max_slot', 'max_cp', 'max_meta', 'slot_bytes', 'work_bytes', 'mode']]
    run = subprocess.run(command, capture_output=True, text=True, timeout=20)
    if run.returncode:
        raise GateError(f"{case['name']}: process failure {run.returncode}; not a killed mutant\n{run.stderr}")
    try:
        got = json.loads(run.stdout)
    except (ValueError, TypeError) as error:
        raise GateError(f"{case['name']}: invalid result JSON") from error
    if not isinstance(got, dict):
        raise GateError('result must be an object')
    return got


def passes(case, got):
    if got.get('rc') != case['expected'] or got.get('writes') != 0:
        return False
    if got['rc']:
        return got.get('unchanged') == 1
    if case.get('op') == 'measure':
        return (got.get('file') == fixture.FILE and got.get('slot') == fixture.SLOT
                and got.get('scratch') == fixture.SLOT and got.get('store', 0) > 0)
    kind, first = case.get('kind', 1), case.get('first', 2)
    expected = dict(file=fixture.FILE, crc=fixture.crc(case['blob']), evidence=1,
                    kind=kind, flags=0 if kind == 1 else 1, first=first, next=4,
                    generation=4 if first == 0 else 5, recovered=0)
    return all(got.get(key) == value for key, value in expected.items())


def killed(case, got, defect):
    # Only an ordinary API success with the specified incorrect behavior counts.
    # A crash, unrelated refusal or malformed output can never kill a mutant.
    if got.get('rc') != 0:
        return False
    if defect == 'accepted':
        return case['expected'] != 0 and got.get('writes') == 0 and got.get('evidence') == 1
    if defect == 'digest':
        return (got.get('crc') != fixture.crc(case['blob'])
                and passes(case, got | {'crc': fixture.crc(case['blob'])}))
    if defect == 'writes':
        return got.get('writes', 0) > 0 and passes(case, got | {'writes': 0})
    raise GateError(f'unknown defect: {defect}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--list', action='store_true', help='read/validate anchors and print witnesses; no builds or writes')
    args = parser.parse_args()
    data = snapshot(); cases = fixture.controls(); changes, by_name = variants(data, cases)
    if args.list:
        for v in changes:
            print(f"{v['name']}: {v['witness']} ({v['defect']})")
        print(f'{len(changes)} reviewed anchors; {len(cases)} unchanged baseline controls')
        return 0
    if args.build_dir is None:
        parser.error('--build-dir required without --list')
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        raise GateError('build directory must be outside the checkout')
    out.mkdir(parents=True, exist_ok=False)
    cc = shlex.split(args.cc)
    if not cc:
        raise GateError('empty compiler command')
    shared = out / 'shared'; shared.mkdir()
    for path, blob in data.items():
        (shared / path.name).write_bytes(blob)
    files = out / 'fixtures'; files.mkdir()
    for case in cases:
        (files / (case['name'] + '.profr')).write_bytes(case['blob'])
    pins = {path.relative_to(ROOT).as_posix(): sha(blob) for path, blob in data.items()}
    (out / 'source-pins.json').write_text(json.dumps(pins, indent=2) + '\n', encoding='utf-8')
    results = []
    try:
        baseline = compile_driver(cc, out / 'baseline', shared, data[IMPL / 'roll_retention_inspect.c'])
        baseline_results = []
        for case in cases:
            physical = invoke(baseline, case, files, True)
            if (physical.get('rc') == 0) != case.get('physical_ok', True) or physical.get('writes') != 0:
                raise GateError(f"invalid independent physical fixture: {case['name']}")
            got = invoke(baseline, case, files)
            baseline_results.append(dict(name=case['name'], physical=physical, observed=got, passed=passes(case, got)))
        (out / 'baseline' / 'results.json').write_text(json.dumps(baseline_results, indent=2) + '\n', encoding='utf-8')
        if not all(r['passed'] for r in baseline_results):
            raise GateError('unchanged baseline failed; see baseline/results.json')
        print(f'BASELINE {len(cases)}/{len(cases)} passed', flush=True)
        for v in changes:
            exe = compile_driver(cc, out / v['name'], shared, v['source'].encode('utf-8'))
            case = by_name[v['witness']]; got = invoke(exe, case, files)
            ok = killed(case, got, v['defect'])
            record = dict(name=v['name'], witness=v['witness'], defect=v['defect'],
                          source_sha256=sha(v['source'].encode('utf-8')), observed=got, killed=ok)
            results.append(record)
            (out / v['name'] / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
            if not ok:
                raise GateError(f"mutant did not exhibit its named defect: {v['name']}: {got}")
            print(f"KILLED {v['name']} by {v['witness']}", flush=True)
    finally:
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        if any(path.read_bytes() != blob for path, blob in data.items()):
            raise GateError('checkout source/test bytes changed during run')
    print(f'PASS {len(changes)}/{len(changes)} inspector sensitivity controls; checkout unchanged')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (GateError, subprocess.TimeoutExpired) as error:
        print(f'FAIL HARNESS: {error}', file=sys.stderr)
        raise SystemExit(2)
