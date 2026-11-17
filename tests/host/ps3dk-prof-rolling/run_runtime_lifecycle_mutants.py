#!/usr/bin/env python3
"""Private lifecycle sensitivity using isolated source copies and real runtime.

Caller owns build authority. A fresh prebuilt rolling_runtime_lifecycle Rust
test binary is required. Native15 and Rust5 must pass on unchanged copied source
before any mutation. Compiler errors, signals, deadlines and unrelated failures
never count as killed mutants. Each copied compiler run records full dependency,
command and executable hashes through run_runtime_lifecycle.py.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DRAIN = Path('sdk/libps3dkprof/src/core/prof_drain.c')
BRIDGE = Path('sdk/libps3dkprof/src/rolling/roll_runtime.c')
RUNNER = Path('tests/host/ps3dk-prof-rolling/run_runtime_lifecycle.py')
CTEST = Path('tests/host/ps3dk-prof-rolling/runtime_lifecycle_test.c')
RTEST = Path('tools/ps3dk-prof/tests/rolling_runtime_lifecycle.rs')
PINS = {
    DRAIN: 'e00ab67def7678c9315fcf26a0842cae21a9a65dd2a5651cab238fc6be84ba20',
    BRIDGE: 'dd91e860e2c615857bcd3310da931441972e8a7c73ea6b0a4657247bb39f2310',
}
RUST_TESTS = {
    'actual_thread_automatic_resource_stop_preserves_first_public_join_and_result',
    'actual_public_and_resource_stop_races_wait_for_one_prepared_frontier',
    'actual_bootstrap_write_and_sync_failures_precede_worker_and_ready',
    'actual_io_close_join_precedence_and_failed_enrollment_are_separate_from_sealed',
    'actual_quick_exit_keeps_only_coherent_open_prefix_without_finalization',
}


class GateError(Exception):
    pass


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def bounded(path, cap=8 << 20):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > cap:
        raise GateError(f'not a bounded regular input: {path}')
    with path.open('rb') as stream:
        blob = stream.read(cap + 1)
    if len(blob) > cap:
        raise GateError(f'input grew past cap: {path}')
    return blob


def inventory():
    paths = [p.relative_to(ROOT) for p in (ROOT / 'sdk/libps3dkprof/src').rglob('*')
             if p.is_file() and p.suffix in ('.c', '.h')]
    paths += [p.relative_to(ROOT) for p in (ROOT / 'tools/ps3dk-prof/src').rglob('*.rs')]
    paths += [Path('sdk/include/ps3dk/prof.h'), RUNNER, CTEST, RTEST,
              Path('tests/host/ps3dk-prof-rolling/runtime_ppu_test.c'),
              Path(__file__).resolve().relative_to(ROOT), Path('tools/ps3dk-prof/Cargo.toml'), Path('tools/Cargo.lock')]
    paths = sorted(set(paths))
    if len(paths) > 1024:
        raise GateError('source inventory exceeds reviewed bound')
    return paths


def snapshot():
    data = {p: bounded(ROOT / p) for p in inventory()}
    if sum(map(len, data.values())) > 32 << 20:
        raise GateError('source snapshot exceeds32MiB')
    for path, digest in PINS.items():
        if sha(data[path].replace(b'\r\n', b'\n')) != digest:
            raise GateError(f'review changed product pin before mutation: {path}')
    return data


def specifications():
    wait = 'while (!LD_ACQ(&p->rolling->stop_prepared)) pal_sleep_us(100);'
    native = lambda case, marker: dict(mode='native', case=case, marker='RUNTIME_PPU_ASSERT[' + marker + ']')
    rust = lambda case, marker: dict(mode='rust', case=case, marker='SEMANTIC[' + marker + ']')
    return [
        ('automatic_consumes_public_claim', DRAIN,
         [('if (ps3dk_prof_test_hooks.rolling_stop_claimed)',
           'if (owner == 2u) ST_REL(&p->shutdown_once, 1u);\n        if (ps3dk_prof_test_hooks.rolling_stop_claimed)')],
         [native('thread-auto', 'public-retrieves-automatic-result')]),
        ('skip_prepared_wait', DRAIN, [(wait, '/* MUTANT: bypass stop preparation publication. */')],
         [native('race-resource-first', 'join-requires-prepared-stop'),
          native('race-public-first', 'frontier-requires-prepared-stop')]),
        ('repeat_preparation_after_lost_claim', DRAIN,
         [(wait + '\n            return;', wait + '\n            /* MUTANT: loser repeats preparation. */')],
         [native('thread-auto', 'claimed-before-prepared')]),
        ('collapse_resource_to_sink_error', BRIDGE,
         [('r->result = PS3DK_PROF_TEST_EROLLRESOURCE;', 'r->result = PS3DK_PROF_ESINKFAIL;')],
         [native('thread-auto', 'public-retrieves-automatic-result')]),
        ('resource_overrides_close_failure', DRAIN,
         [('if (p->snk.close_failed) return PS3DK_PROF_ECLOSE;\n        if (p->rolling->result) return p->rolling->result;',
           'if (p->rolling->result) return p->rolling->result;\n        if (p->snk.close_failed) return PS3DK_PROF_ECLOSE;')],
         [native('resource-close-join', 'fault-primary-result')]),
        ('discard_separate_join_error', BRIDGE,
         [('r->record.join_result = p->join_failed ? PS3DK_PROF_ETHREAD : 0;', 'r->record.join_result = 0;')],
         [native('join-only', 'typed-failure-precedence')]),
        ('enroll_failed_sealed_capture', DRAIN,
         [('if (result == PS3DK_PROF_OK && p->retention_keep &&\n            pal_retain_complete_kind',
           'if (p->retention_keep &&\n            pal_retain_complete_kind')],
         [native('join-only', 'no-enrollment-on-failure-or-resource')]),
        ('worker_enrolls_before_join', DRAIN,
         [('rc = p_quiesce_freeze_seal(p);\n    __atomic_store_n(&p->thread_result',
           'rc = p_quiesce_freeze_seal(p);\n    (void)retain_completed(p, rc);\n    __atomic_store_n(&p->thread_result')],
         [native('eligible-thread', 'enrollment-after-join')]),
        ('publish_finalization_before_join', DRAIN,
         [('if (pal_thread_join(p->drain_thread) != 0) {',
           'publish_finalization(p, PS3DK_PROF_OK);\n    if (pal_thread_join(p->drain_thread) != 0) {')],
         [native('thread-auto', 'join-before-publication')]),
        ('quick_exit_fabricates_sealed', DRAIN,
         [('if (p->rolling) (void)p_roll_flush(p); /* coherent OPEN prefix, never finish/enroll */',
           'if (p->rolling) {\n        pfr_view v; (void)p_roll_flush(p);\n'
           '        if (pfr_get_view(p->rolling->store, &v) == PFR_OK) {\n'
           '            pfr_transition t = {v.generation, v.next_chunk, v.bound_payload_bytes, PFR_SEALED, PFR_REASON_NONE};\n'
           '            (void)pfr_publish_state(p->rolling->store, &t);\n        }\n    }')],
         [rust('actual_quick_exit_keeps_only_coherent_open_prefix_without_finalization', 'quick-exit-keeps-open')]),
        ('omit_successful_preterminal_observation', BRIDGE,
         [('(void)observe(p, rc);\n    if (rc != PFR_OK) return p->rolling->result;',
           'if (rc != PFR_OK) (void)observe(p, rc);\n    if (rc != PFR_OK) return p->rolling->result;')],
         [rust('actual_io_close_join_precedence_and_failed_enrollment_are_separate_from_sealed', 'acknowledged-root-revision')]),
    ]


def variants(data):
    parsed = ast.parse(data[RUNNER].decode())
    cases = next(ast.literal_eval(node.value) for node in parsed.body if isinstance(node, ast.Assign)
                 and any(isinstance(target, ast.Name) and target.id == 'CASES' for target in node.targets))
    if len(cases) != 15 or len(set(cases)) != 15:
        raise GateError('review native15 inventory before changing it')
    result = []
    for name, path, edits, witnesses in specifications():
        original = data[path].decode().replace('\r\n', '\n'); changed = original
        for before, after in edits:
            if changed.count(before) != 1:
                raise GateError(f'{name}: mutation anchor absent/ambiguous: {before!r}')
            changed = changed.replace(before, after, 1)
        if changed == original:
            raise GateError(f'{name}: unchanged mutant')
        for witness in witnesses:
            if witness['mode'] == 'native':
                if witness['case'] not in cases or witness['marker'][19:-1] not in data[CTEST].decode():
                    raise GateError(f'{name}: absent native witness {witness}')
            elif witness['case'] not in RUST_TESTS or witness['marker'] not in data[RTEST].decode():
                raise GateError(f'{name}: absent Rust witness {witness}')
        result.append(dict(name=name, path=path, source=changed.encode(), witnesses=witnesses))
    return result, cases


def command_run(command, directory, label, timeout, env=None):
    stdout = directory / (label + '.stdout'); stderr = directory / (label + '.stderr')
    with stdout.open('xb') as out, stderr.open('xb') as err:
        process = subprocess.run(command, stdout=out, stderr=err, timeout=timeout, env=env)
    text = (bounded(stdout, 4 << 20) + bounded(stderr, 4 << 20)).decode(errors='replace').replace('\r\n', '\n')
    return process.returncode, text


def materialize(directory, data, variant=None):
    tree = directory / 'source'; tree.mkdir(parents=True)
    # Keep copies small: Rust is prebuilt, and only its hashes are needed here.
    for path, blob in data.items():
        if path.parts[0] == 'tools':
            continue
        target = tree / path; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(blob)
    if variant:
        (tree / variant['path']).write_bytes(variant['source'])
    return tree


def build(directory, tree, cc, native_baseline=False):
    build_dir = directory / 'build'
    command = [sys.executable, str(tree / RUNNER), '--build-dir', str(build_dir), '--cc', cc]
    if native_baseline:
        command.append('--run-native')
    rc, _ = command_run(command, directory, 'build', 1200)
    if rc:
        raise GateError(f'{directory.name}: build or unchanged native baseline failed ({rc}); not a killed mutant')
    receipt = json.loads(bounded(build_dir / 'build-receipt.json'))
    if receipt.get('compile_rc') != 0 or not receipt.get('input_hashes_unchanged') or not receipt.get('dependency_inventory_unchanged'):
        raise GateError('compiler provenance receipt invalid')
    exe = build_dir / 'runtime-lifecycle'
    if sha(bounded(exe, 256 << 20)) != receipt['executable_sha256']:
        raise GateError('runtime executable does not match build receipt')
    if native_baseline:
        results = json.loads(bounded(build_dir / 'native-results.json'))
        if len(results) != 15 or any(row['rc'] for row in results):
            raise GateError('native15 baseline not green')
    return exe


def invoke_rust(binary, fixture, directory, witness=None):
    # Runtime capture paths have a genuine255-byte bound. Keep the fixture's
    # nested unique directory short even when the external build root is long.
    # Failure evidence is retained and its owned location is recorded here.
    scratch = Path(tempfile.mkdtemp(prefix='pfr-runtime-mutant-')).resolve()
    (directory / 'scratch-location.txt').write_text(str(scratch) + '\n')
    env = os.environ.copy(); env['PS3DK_PROF_RUNTIME_LIFECYCLE'] = str(fixture)
    for name in ('TMP', 'TEMP', 'TMPDIR'):
        env[name] = str(scratch)
    command = [str(binary)] + ([witness, '--exact'] if witness else [])
    command += ['--ignored', '--nocapture', '--test-threads=1']
    result = command_run(command, directory, 'test', 600, env)
    if not any(scratch.iterdir()):
        scratch.rmdir()
    return result


def killed(rc, text, witness):
    if any(term in text for term in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:', 'deadline exceeded')):
        return False
    if witness['mode'] == 'native':
        return (rc == 1 and text.count('RUNTIME_PPU_ASSERT[') == 1
                and re.search(r'^' + re.escape(witness['marker']) + r': .+$', text, re.MULTILINE) is not None)
    return (rc == 101 and witness['marker'] in text and text.count('panicked at ') == 1
            and re.search(r'rolling_runtime_lifecycle\.rs:\d+:\d+', text) is not None
            and 'test result: FAILED. 0 passed; 1 failed; 0 ignored;' in text
            and 'real runtime assertion failed' not in text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--rust-test-bin', type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args(); data = snapshot(); changes, cases = variants(data)
    if args.check_only:
        for v in changes:
            print(v['name'] + ': ' + ', '.join(w['case'] + ' -> ' + w['marker'] for w in v['witnesses']))
        print(f'{len(changes)} mutations / {sum(len(v["witnesses"]) for v in changes)} witnesses; native15 + Rust5 baseline required; no subprocesses')
        return 0
    if args.build_dir is None or args.rust_test_bin is None:
        parser.error('--build-dir and --rust-test-bin required without --check-only')
    out = args.build_dir.resolve(); binary = args.rust_test_bin.resolve()
    if out == ROOT or ROOT in out.parents:
        raise GateError('use fresh external build directory')
    rust_bytes = bounded(binary, 256 << 20)
    out.mkdir(parents=True, exist_ok=False)
    pins = {path.as_posix(): sha(blob) for path, blob in data.items()}
    (out / 'source-pins.json').write_text(json.dumps(dict(inputs=pins, rust_test_binary=str(binary),
        rust_test_sha256=sha(rust_bytes)), indent=2) + '\n')
    results = []
    try:
        rc, listing = command_run([str(binary), '--list', '--ignored'], out, 'inventory', 30)
        if rc or set(re.findall(r'^(\w+): test$', listing, re.MULTILINE)) != RUST_TESTS:
            raise GateError('fresh Rust test binary must contain exactly the five reviewed tests')
        directory = out / 'baseline'; tree = materialize(directory, data)
        fixture = build(directory, tree, args.cc, True)
        rc, text = invoke_rust(binary, fixture, directory)
        if rc or 'test result: ok. 5 passed; 0 failed; 0 ignored;' not in text:
            raise GateError('unchanged Rust5 baseline failed; no mutation evidence is valid')
        print('BASELINE native15/15 and Rust5/5 passed', flush=True)
        for v in changes:
            directory = out / v['name']; tree = materialize(directory, data, v)
            fixture = build(directory, tree, args.cc)
            record = dict(name=v['name'], source_sha256=sha(v['source']), witnesses=[])
            for i, witness in enumerate(v['witnesses']):
                evidence = directory / f'witness-{i}'; evidence.mkdir()
                if witness['mode'] == 'native':
                    artifacts = evidence / 'artifacts'; artifacts.mkdir()
                    rc, text = command_run([str(fixture), witness['case'], str(artifacts)], evidence, 'test', 60)
                else:
                    rc, text = invoke_rust(binary, fixture, evidence, witness['case'])
                observed = dict(witness, rc=rc, killed=killed(rc, text, witness)); record['witnesses'].append(observed)
                (directory / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
                if not observed['killed']:
                    results.append(record)
                    raise GateError(f"{v['name']}: intended semantic witness absent; inspect retained logs, never count a timeout/crash")
                print(f"KILLED {v['name']} by {witness['marker']}", flush=True)
            results.append(record)
    finally:
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        if inventory() != sorted(data) or any(bounded(ROOT / p) != blob for p, blob in data.items()) or bounded(binary, 256 << 20) != rust_bytes:
            raise GateError('live checkout or prebuilt test binary changed during isolated run')
    print(f'PASS {len(changes)} lifecycle mutations / {sum(len(v["witnesses"]) for v in changes)} named witnesses; checkout unchanged')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (GateError, subprocess.TimeoutExpired, OSError, ValueError) as error:
        print(f'FAIL HARNESS: {error}', file=sys.stderr)
        raise SystemExit(2)
