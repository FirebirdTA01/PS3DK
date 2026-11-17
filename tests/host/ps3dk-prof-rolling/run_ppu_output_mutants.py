#!/usr/bin/env python3
"""Isolated actual pfo/pfp/pfr sensitivity. Caller owns compiler/build authority.

Requires a freshly built rolling_ppu_c_output Rust test executable from this
checkout. Its hash and all C/test source hashes are recorded; the unchanged five
tests must pass first. Only the named semantic assertion kills each mutant.
Compiler errors, crashes, timeouts and unrelated failures are harness failures.
--check-only validates the pinned sources/anchors without writes or subprocesses.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
IMPL = ROOT / 'sdk/libps3dkprof/src/rolling'
RUST = ROOT / 'tools/ps3dk-prof/tests/rolling_ppu_c_output.rs'
PINS = {
    'roll_output.c': '8e54c64c83cc9242e36dd9d555a88af82af715975bdb6973946abccb5feed787',
    # Verified shared-family extraction: standalone pfp_prepare still calls
    # core_accept(external_metadata=0), then the same PFRCKP01 envelope;
    # core_sections(counts=NULL) preserves standalone 120-byte lane rows.
    # All eight mutation sites remain in unchanged roll_output.c.
    'roll_ppu.c': '37c3dc29c1a486c69816840ee34c3132be570b25a2785920675052a64a052245',
    'roll_ppu_internal.h': '6282c7128aeee1fcc738cb62b0c2977acbe180b8114d408a86469476f5859230',
    'roll_store.c': '4ce68eadd1f52a5307d90b211dc93abe2b497440df489da34b22767bd578951d',
}
PREFIX = 'actual_c_ppu_output_'
ROTATE = PREFIX + 'rotates_and_preserves_useful_retained_spans'
BOOT = PREFIX + 'pending_bootstrap_and_empty_flush_are_explicit'
CAPS = PREFIX + 'every_cap_stops_before_candidate_and_preserves_tail'
FAULT = PREFIX + 'every_observed_write_and_sync_fault_preserves_known_root'
SHORT = PREFIX + 'short_positive_io_and_zero_progress_are_distinct'
TESTS = {ROTATE, BOOT, CAPS, FAULT, SHORT}


class GateError(Exception):
    pass


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def read_bounded(path, cap=1048576):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > cap:
        raise GateError(f'not a bounded regular input: {path}')
    with path.open('rb') as stream:
        blob = stream.read(cap + 1)
    if len(blob) > cap:
        raise GateError(f'input grew beyond cap: {path}')
    return blob


def specifications():
    rotation = 'if(n>o->b.payload_capacity-o->v.pending_bytes){rc=pfo_flush(o);if(rc!=PFR_OK)return rc;}'
    return [
        ('acknowledge_failed_rotation', FAULT, 'C_SEMANTIC[accepted-state-immutable]', [
            (rotation, 'if(n>o->b.payload_capacity-o->v.pending_bytes){rc=pfo_flush(o);if(rc!=PFR_OK){'
             'o->v.accepted_next_chunk=next;o->v.accepted_payload_bytes=bytes;return rc;}}')]),
        ('successor_as_segment_start', ROTATE, 'SEMANTIC[checkpoint-precedes-payload]', [
            ('rc=pfr_commit(o->store,o->b.checkpoints[0],o->start_bytes,o->b.payload,o->v.pending_bytes);',
             'if(!o->v.terminal){memcpy(o->b.checkpoints[0],o->b.checkpoints[1],o->accepted_bytes);o->start_bytes=o->accepted_bytes;}\n'
             '    rc=pfr_commit(o->store,o->b.checkpoints[0],o->start_bytes,o->b.payload,o->v.pending_bytes);')]),
        ('rotate_before_resource_preflight', CAPS, 'C_SEMANTIC[refused-pending-immutable]', [
            ('memset(&prepared,0,sizeof prepared);', rotation + '\n    memset(&prepared,0,sizeof prepared);')]),
        ('skip_resource_tail_flush', CAPS, 'SEMANTIC[resource-tail-before-terminal]', [
            ('rc=pfo_flush(o);if(rc!=PFR_OK)return rc;\n    rc=pfr_get_view(o->store,&v);',
             'if(o->v.phase!=PFO_STOPPING){rc=pfo_flush(o);if(rc!=PFR_OK)return rc;}\n    rc=pfr_get_view(o->store,&v);')]),
        ('collapse_resource_reason', CAPS, 'SEMANTIC[resource-reason-binding]', [
            ('o->v.phase=PFO_STOPPING;o->v.reason=reason;', 'o->v.phase=PFO_STOPPING;o->v.reason=reason?PFR_CHECKPOINT_BYTES:0;')]),
        ('publish_each_ram_accept', BOOT, 'SEMANTIC[ram-before-publication]', [
            ('return PFR_OK;\n}\nint pfo_finish', 'return pfo_flush(o);\n}\nint pfo_finish')]),
        ('permit_poison_reentry', FAULT, 'C_SEMANTIC[poison-refuses-reentry]', [
            ('if(!o||o->ready!=READY||o->v.phase==PFO_POISONED)return PFR_FAILED;',
             'if(!o||o->ready!=READY)return PFR_FAILED;')]),
        ('terminal_root_left_open', ROTATE, 'SEMANTIC[terminal-root-declaration]', [
            ('t.state=o->v.phase==PFO_STOPPING?PFR_STOPPED_RESOURCE:PFR_SEALED;',
             't.state=o->v.phase==PFO_STOPPING?PFR_STOPPED_RESOURCE:PFR_OPEN;')]),
    ]


def snapshot():
    files = [IMPL / name for name in ('roll_output.c', 'roll_output.h', 'roll_ppu.c',
             'roll_ppu.h', 'roll_ppu_internal.h', 'roll_store.c', 'roll_store.h')]
    files += [HERE / 'ppu_output_fixture.c', HERE / 'run_ppu_output.py', Path(__file__).resolve(),
              RUST, RUST.parent / 'common/mod.rs']
    data = {p: read_bounded(p) for p in files}
    for name, digest in PINS.items():
        # Git checkout newline style may differ between Windows and WSL.
        if sha(data[IMPL / name].replace(b'\r\n', b'\n')) != digest:
            raise GateError(f'review production source/anchors before changing pin: {name}')
    return data


def variants(data):
    source = data[IMPL / 'roll_output.c'].decode('utf-8').replace('\r\n', '\n')
    result = []
    for name, witness, marker, edits in specifications():
        changed = source
        for before, after in edits:
            if changed.count(before) != 1:
                raise GateError(f'{name}: missing/ambiguous mutation anchor {before!r}')
            changed = changed.replace(before, after, 1)
        if marker.startswith('C_'):
            if marker[11:-1] not in data[HERE / 'ppu_output_fixture.c'].decode('utf-8'):
                raise GateError(f'{name}: missing C semantic witness {marker}')
        elif marker not in data[RUST].decode('utf-8'):
            raise GateError(f'{name}: missing Rust semantic witness {marker}')
        result.append(dict(name=name, witness=witness, marker=marker, source=changed.encode('utf-8')))
    if len({v['source'] for v in result}) != len(result) or any(v['source'] == source.encode() for v in result):
        raise GateError('mutation sources must be distinct changes')
    return result


def command_run(command, directory, name, timeout, env=None):
    stdout = directory / (name + '.stdout')
    stderr = directory / (name + '.stderr')
    with stdout.open('xb') as out, stderr.open('xb') as err:
        process = subprocess.run(command, stdout=out, stderr=err, timeout=timeout, env=env)
    text = (read_bounded(stdout, 4 << 20) + read_bounded(stderr, 4 << 20)).decode('utf-8', errors='replace')
    return process.returncode, text


def compile_driver(cc, directory, shared, source):
    directory.mkdir()
    (directory / 'roll_output.c').write_bytes(source)
    exe = directory / ('rolling-ppu-output-fixture.exe' if sys.platform == 'win32' else 'rolling-ppu-output-fixture')
    command = cc + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-I', str(shared), str(shared / 'ppu_output_fixture.c'), str(directory / 'roll_output.c'),
                    str(shared / 'roll_ppu.c'), str(shared / 'roll_store.c'), '-o', str(exe)]
    rc, _ = command_run(command, directory, 'build', 180)
    if rc:
        raise GateError(f'{directory.name}: compiler failure {rc}; never a killed mutant')
    return exe


def invoke(test_binary, fixture, directory, witness=None):
    env = os.environ.copy()
    env['PS3DK_PROF_ROLLING_PPU_OUTPUT'] = str(fixture)
    scratch = directory / 'scratch'; scratch.mkdir()
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        env[key] = str(scratch)
    command = [str(test_binary)] + ([witness, '--exact'] if witness else [])
    command += ['--ignored', '--nocapture', '--test-threads=1']
    return command_run(command, directory, 'test', 600, env)


def detected(rc, text, variant):
    if rc != 101:
        return False
    # The C fixture deliberately exits 1 for a named failed assertion. Rust
    # propagates that marker only after checking the child's exact exit code.
    # An abort/signal or an unrelated harness CHECK uses a different diagnostic.
    if any(term in text for term in ('fixture infrastructure failure', 'fixture timeout',
                                     'AddressSanitizer', 'runtime error:', 'fatal runtime error')):
        return False
    if text.count("panicked at ") != 1 or not re.search(r'rolling_ppu_c_output\.rs:\d+:\d+', text):
        return False
    if variant['marker'].startswith('C_') and 'named C semantic assertion:' not in text:
        return False
    return (variant['marker'] in text
            and re.search(r'test result: FAILED\. 0 passed; 1 failed; 0 ignored;', text) is not None
            and variant['witness'] in text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--rust-test-bin', type=Path, help='freshly built rolling_ppu_c_output integration test executable')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    data = snapshot(); changes = variants(data)
    if args.check_only:
        for v in changes:
            print(f"{v['name']}: {v['witness']} -> {v['marker']}")
        print(f'{len(changes)} exact mutations; five unchanged baseline tests; no subprocesses')
        return 0
    if args.build_dir is None or args.rust_test_bin is None:
        parser.error('--build-dir and --rust-test-bin required without --check-only')
    out = args.build_dir.resolve(); test_binary = args.rust_test_bin.resolve()
    if out == ROOT or ROOT in out.parents:
        raise GateError('use a fresh build directory outside the checkout')
    binary = read_bounded(test_binary, 256 << 20)
    cc = shlex.split(args.cc)
    if not cc:
        raise GateError('empty compiler command')
    out.mkdir(parents=True, exist_ok=False)
    shared = out / 'shared'; shared.mkdir()
    for path, blob in data.items():
        (shared / path.name).write_bytes(blob)
    pins = {path.relative_to(ROOT).as_posix(): sha(blob) for path, blob in data.items()}
    (out / 'source-pins.json').write_text(json.dumps({'sources': pins, 'rust_test_binary': str(test_binary),
        'rust_test_binary_sha256': sha(binary)}, indent=2) + '\n', encoding='utf-8')
    results = []
    try:
        rc, listing = command_run([str(test_binary), '--list', '--ignored'], out, 'inventory', 30)
        if rc or set(re.findall(r'^(\w+): test$', listing, re.MULTILINE)) != TESTS:
            raise GateError('prebuilt binary must expose exactly the five reviewed ignored tests')
        baseline = compile_driver(cc, out / 'baseline', shared, data[IMPL / 'roll_output.c'])
        rc, text = invoke(test_binary, baseline, out / 'baseline')
        if rc or 'test result: ok. 5 passed; 0 failed; 0 ignored;' not in text:
            raise GateError('unchanged baseline failed; no mutation results are valid')
        print('BASELINE 5/5 passed', flush=True)
        for v in changes:
            directory = out / v['name']
            exe = compile_driver(cc, directory, shared, v['source'])
            rc, text = invoke(test_binary, exe, directory, v['witness'])
            killed = detected(rc, text, v)
            record = {key: v[key] for key in ('name', 'witness', 'marker')}
            record.update(source_sha256=sha(v['source']), test_rc=rc, killed=killed)
            results.append(record)
            (directory / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
            if not killed:
                raise GateError(f"{v['name']}: missing named semantic failure; see retained test/producer evidence")
            print(f"KILLED {v['name']} by {v['marker']}", flush=True)
    finally:
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
        if any(path.read_bytes() != blob for path, blob in data.items()) or read_bounded(test_binary, 256 << 20) != binary:
            raise GateError('checkout sources/tests or prebuilt test binary changed during run')
    print(f'PASS {len(changes)}/{len(changes)} named semantic controls; checkout unchanged')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (GateError, subprocess.TimeoutExpired, OSError) as error:
        print(f'FAIL HARNESS: {error}', file=sys.stderr)
        raise SystemExit(2)
