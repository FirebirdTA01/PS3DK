#!/usr/bin/env python3
"""D6 sensitivity on isolated actual runtime/SPU sources; parent owns builds.

Requires unchanged native8 + Rust5 baselines. Only the designated semantic
assertion counts; compiler errors, signals, deadlines and unrelated failures do
not. Product pins were reviewed after parent gate120 GREEN, not by this runner.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CORE = Path('sdk/libps3dkprof/src/core/prof_core.c')
BRIDGE = Path('sdk/libps3dkprof/src/rolling/roll_runtime.c')
SPU = Path('sdk/libps3dkprof/src/spu/psp_ppu.c')
EXT = Path('sdk/libps3dkprof/src/spu/ext_runtime.c')
RUNNER = Path('tests/host/ps3dk-prof-rolling/run_runtime_spu.py')
CTEST = Path('tests/host/ps3dk-prof-rolling/runtime_spu_test.c')
RTEST = Path('tools/ps3dk-prof/tests/rolling_runtime_spu.rs')
# Normalized LF pins reviewed after parent gate101 ASan/UBSan and Rust5 GREEN.
# Bridge/SPU retain gate97 pins; ext_runtime has the gate101 inline boundary.
# CORE repinned after gate107: immutable capture-format publication for live
# refusal only. Mandatory isolated baselines still run first.
# CORE/BRIDGE repinned after gate120 and review against a79284960: tagged
# PPU/combined/schema2 selection. Combined witnesses and baseline stay unchanged.
APPROVED_PINS = {
    'sdk/libps3dkprof/src/core/prof_core.c': 'f7e69aee0fe5f9fa147e9cbce1b5f7ce673087fc9b0ffa39d5f1f56d1989406c',
    'sdk/libps3dkprof/src/rolling/roll_runtime.c': '2d9250d67d171b38c5640142f4db15401c42b71ed779cdd02942629411cded46',
    'sdk/libps3dkprof/src/spu/psp_ppu.c': '202ea84699da83f036a9fabc39f3b444283113c6d901d1abb24b5499776dcada',
    'sdk/libps3dkprof/src/spu/ext_runtime.c': '2505d2676406d920172bf950359c97bfee82d69c2cebde8bcc61ebaf54ad4714',
}
PIN_PATHS = (CORE, BRIDGE, SPU, EXT)
NATIVE_CASES = ('append-rotation', 'append-crossing', 'append-resource', 'rotation', 'crossing', 'resource', 'append-inline-resource', 'inline-resource')
RUST_TESTS = {
    'actual_spu_append_control_validates_original_producer_truth',
    'actual_runtime_spu_rotates_without_reconstructing_evicted_events',
    'actual_runtime_spu_begin_crosses_eviction_as_censored_support',
    'actual_runtime_spu_resource_refusal_precedes_producer_acknowledgement',
    'actual_runtime_spu_lane_end_services_inline_resource_stop',
}


class GateError(Exception):
    pass


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def bounded(path, cap=8 << 20):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > cap:
        raise GateError(f'not a bounded regular input: {path}')
    with path.open('rb') as f:
        b = f.read(cap + 1)
    if len(b) > cap:
        raise GateError(f'input grew beyond cap: {path}')
    return b


def inventory():
    paths = {p.relative_to(ROOT) for p in (ROOT / 'sdk/libps3dkprof/src').rglob('*')
             if p.is_file() and p.suffix in ('.c', '.h')}
    paths.update(p.relative_to(ROOT) for p in (ROOT / 'tools/ps3dk-prof/src').rglob('*.rs'))
    paths.update(p.relative_to(ROOT) for p in HERE.glob('runtime_spu*') if p.is_file())
    paths.update(p.relative_to(ROOT) for p in (ROOT / 'tools/ps3dk-prof/tests/common').rglob('*.rs'))
    paths.update((Path('sdk/include/ps3dk/prof.h'), RUNNER, CTEST, RTEST,
                  Path('tests/host/ps3dk-prof-spu-int/i2_test.c'),
                  Path('tools/ps3dk-prof/Cargo.toml'), Path('tools/Cargo.lock'),
                  Path(__file__).resolve().relative_to(ROOT)))
    if len(paths) > 2048:
        raise GateError('source inventory exceeds reviewed bound')
    return sorted(paths)


def snapshot():
    data = {p: bounded(ROOT / p) for p in inventory()}
    if sum(map(len, data.values())) > 64 << 20:
        raise GateError('source snapshot exceeds 64MiB')
    return data


def specifications():
    native = lambda case, marker: dict(mode='native', case=case, marker=f'RUNTIME_SPU_ASSERT[{marker}]')
    combined_setter = ('int ps3dk_prof_test_rolling_combined_set(const ps3dk_prof_test_rolling_options *o, const pfm_config *m)\n'
                       '{\n    int rc = gate_enter(G_CONFIGURING);\n    if (rc) return rc;\n'
                       '    if (!o || o->size != sizeof *o || o->version != 1 || o->reserved || o->enabled > 1 ||\n'
                       '        (o->enabled && !m)')
    return [
        dict(name='missing_inline_consumer_boundary', edits=[(EXT,
             '#ifdef P_ROLLING_RUNTIME\n'
             '    if (p->rolling) {\n'
             '        /* The lane-end request has released spu_mutex. Use the APP consumer\n'
             '         * boundary, including tail commit and deferred resource stopping;\n'
             '         * neither action is safe from an encoder or extension callback. */\n'
             '        (void)ps3dk_prof_drain();\n'
             '        return;\n'
             '    }\n'
             '#endif\n',
             '/* MUTANT: omit the private inline rolling consumer boundary. */\n')],
             witness=native('inline-resource', 'inline-resource-automatic-finish')),
        dict(name='read_ack_on_refusal', edits=[(SPU,
             'if (l->state == PSP_L_FINAL || p->sink_failed) {\n                l->dphase = 0;',
             'if (l->state == PSP_L_FINAL || p->sink_failed) {\n'
             '                if (p->sink_failed) m->st64(m->ctx, l->ea_ctl + PSP_A_READ_IDX, l->w_snap);\n'
             '                l->dphase = 0;')], witness=native('resource', 'resource-before-read-ack')),
        dict(name='event_ledger_on_refusal', edits=[(SPU,
             'if (!emit(p, C_SPU_EVENTS))\n        return;',
             'if (!emit(p, C_SPU_EVENTS)) {\n'
             '        l->emitted_next += n; l->emitted_events += n;\n        return;\n    }')],
             witness=native('resource', 'resource-before-event-ledger-ack')),
        dict(name='resource_becomes_sink_error', edits=[(BRIDGE,
             'r->result = P_ROLL_RESOURCE;', 'r->result = PS3DK_PROF_ESINKFAIL;')],
             witness=native('resource', 'SPU-resource-not-sink-failure')),
        dict(name='ppu_prepare_for_combined', edits=[(BRIDGE,
             'if (m) r->buffers.prepare = pfm_prepare;\n#endif\n#ifdef P_ROLLING_SCHEMA2',
             'if (m) r->buffers.prepare = pfp_prepare;\n#endif\n#ifdef P_ROLLING_SCHEMA2')],
             witness=native('rotation', 'real-combined-init')),
        dict(name='mask_result_original_flags', edits=[(BRIDGE,
             '(uint32_t)head[14] << 16 | (uint32_t)head[15] << 24;',
             '(uint32_t)head[14] << 16 | (uint32_t)head[15] << 24;\n    r->record.original_flags &= ~6u;')],
             witness=native('rotation', 'whole-original-flags-and-result')),
        dict(name='mask_initial_checkpoint_flags', edits=[(BRIDGE,
             'if (rc) return PS3DK_PROF_EINVAL;\n    if (pfr_file_bind',
             'if (rc) return PS3DK_PROF_EINVAL;\n'
             '    if (r->selection.kind == P_ROLL_COMBINED) {\n'
             '        uint8_t *bad = (uint8_t *)r->initial;\n'
             '        p_le32(bad + 28, r->record.original_flags & ~6u);\n'
             '        p_le32(bad + initial - 4, p_crc32(bad, initial - 4));\n'
             '    }\n    if (pfr_file_bind')],
             witness=dict(mode='rust', case='actual_runtime_spu_rotates_without_reconstructing_evicted_events',
                          marker='C runtime checkpoint vs original-prefix Rust state')),
        # One intentionally joint control: both redundant capacity gates must
        # be bypassed. Never label it two independent guard kills.
        dict(name='allow_conflicting_ppu_capacities', edits=[(CORE,
             combined_setter + ' || o->ppu.incarnations || o->ppu.descriptors || o->ppu.stack_rows ||\n'
             '        o->ppu.frame_identities || o->ppu.metadata_bytes || o->ppu.checkpoint_bytes)',
             combined_setter + ')'), (BRIDGE,
             'if (selection && selection->kind != P_ROLL_PPU &&\n'
             '             (o->ppu.incarnations || o->ppu.descriptors || o->ppu.stack_rows ||\n'
             '              o->ppu.frame_identities || o->ppu.metadata_bytes || o->ppu.checkpoint_bytes))\n'
             '        return PS3DK_PROF_EINVAL;',
             'if (selection && selection->kind != P_ROLL_PPU && selection->kind != P_ROLL_COMBINED &&\n'
             '             (o->ppu.incarnations || o->ppu.descriptors || o->ppu.stack_rows ||\n'
             '              o->ppu.frame_identities || o->ppu.metadata_bytes || o->ppu.checkpoint_bytes))\n'
             '        return PS3DK_PROF_EINVAL;')],
             witness=native('rotation', 'conflicting-capacities-refused')),
    ]


def variants(data):
    result = []
    for spec in specifications():
        sources = {}
        for path, before, after in spec['edits']:
            source = sources.get(path, data[path].decode().replace('\r\n', '\n'))
            if source.count(before) != 1:
                raise GateError(f'{spec["name"]}: missing/ambiguous mutation anchor {before!r}')
            sources[path] = source.replace(before, after, 1)
        witness = spec['witness']
        if witness['mode'] == 'native':
            if witness['case'] not in NATIVE_CASES or witness['marker'][19:-1] not in data[CTEST].decode():
                raise GateError(f'{spec["name"]}: absent named native witness')
        elif witness['case'] not in RUST_TESTS or witness['marker'] not in data[RTEST].decode():
            raise GateError(f'{spec["name"]}: absent Rust semantic witness')
        for path, source in sources.items():
            if source == data[path].decode().replace('\r\n', '\n'):
                raise GateError(f'{spec["name"]}: mutation did not change {path}')
        result.append(dict(name=spec['name'], sources={p: s.encode() for p, s in sources.items()}, witness=witness))
    return result


def run(command, directory, label, seconds, env=None):
    out, err = directory / (label + '.stdout'), directory / (label + '.stderr')
    with out.open('xb') as stdout, err.open('xb') as stderr:
        child = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=env, start_new_session=True)
        start = time.monotonic()
        try:
            while child.poll() is None:
                if time.monotonic() - start > seconds or max(out.stat().st_size, err.stat().st_size) > 2 << 20:
                    raise GateError(f'{label}: deadline/log bound, never a killed mutant')
                time.sleep(.02)
            if max(out.stat().st_size, err.stat().st_size) > 2 << 20:
                raise GateError(f'{label}: output exceeds bound')
            return child.returncode, (bounded(out, 2 << 20) + bounded(err, 2 << 20)).decode(errors='replace').replace('\r\n', '\n')
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=10)


def materialize(directory, data, variant=None):
    tree = directory / 'source'; tree.mkdir(parents=True)
    for path, blob in data.items():
        if path.parts[0] == 'tools':
            continue  # Rust is prebuilt; its source hashes remain in provenance.
        target = tree / path; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(blob)
    if variant:
        for path, blob in variant['sources'].items():
            (tree / path).write_bytes(blob)
    return tree


def build(directory, tree, cc):
    out = directory / 'build'
    rc, _ = run([sys.executable, str(tree / RUNNER), '--build-dir', str(out), '--cc', cc], directory, 'build', 1800)
    if rc:
        raise GateError(f'{directory.name}: compiler/input gate failed; never a killed mutant')
    receipt = json.loads(bounded(out / 'receipt.json'))
    if receipt.get('compile_rc') != 0 or not receipt.get('inputs_unchanged'):
        raise GateError('compiler receipt does not establish stable successful build')
    exe = out / 'runtime-spu'
    if sha(bounded(exe, 256 << 20)) != receipt['executable_sha256']:
        raise GateError('runtime executable differs from build receipt')
    return exe


def native(fixture, directory, case):
    artifacts = directory / 'artifacts'; artifacts.mkdir()
    return run([str(fixture), case, str(artifacts)], directory, 'test', 60)


def rust(binary, fixture, directory, case=None):
    scratch = Path(tempfile.mkdtemp(prefix='pfr-d6-mutant-')).resolve()
    (directory / 'scratch-location.txt').write_text(str(scratch) + '\n')
    env = os.environ.copy(); env['PS3DK_PROF_RUNTIME_SPU'] = str(fixture)
    for name in ('TMP', 'TEMP', 'TMPDIR'):
        env[name] = str(scratch)
    command = [str(binary)] + ([case, '--exact'] if case else []) + ['--ignored', '--nocapture', '--test-threads=1']
    result = run(command, directory, 'test', 600, env)
    if not any(scratch.iterdir()):
        scratch.rmdir()
    return result


def killed(rc, text, witness):
    if any(x in text for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:', 'deadline', 'log bound')):
        return False
    if witness['mode'] == 'native':
        return (rc == 1 and text.count('RUNTIME_SPU_ASSERT[') == 1
                and re.search(r'^' + re.escape(witness['marker']) + r': .+$', text, re.MULTILINE) is not None)
    return (rc == 101 and text.count('panicked at ') == 1 and witness['marker'] in text
            and re.search(r'rolling_runtime_spu\.rs:\d+:\d+', text) is not None
            and 'test result: FAILED. 0 passed; 1 failed; 0 ignored;' in text
            and 'native assertion failed' not in text and 'RUNTIME_SPU_ASSERT[' not in text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--rust-test-bin', type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args(); data = snapshot(); changes = variants(data)
    candidates = {p.as_posix(): sha(data[p].replace(b'\r\n', b'\n')) for p in PIN_PATHS}
    if args.check_only:
        print(json.dumps(dict(observed_pins=candidates, approved_pins_match=APPROVED_PINS == candidates), indent=2))
        for v in changes:
            print(f'{v["name"]}: {v["witness"]["case"]} -> {v["witness"]["marker"]}')
        print('8 controls; native8 + Rust5 mandatory; no subprocess/build; source pins reviewed after gate120')
        return 0
    if os.name != 'posix':
        raise GateError('normal execution requires Linux/WSL')
    if not APPROVED_PINS or APPROVED_PINS != candidates:
        raise GateError('approved product pins absent/changed; wait for parent GREEN and source review')
    if args.build_dir is None or args.rust_test_bin is None:
        parser.error('--build-dir and --rust-test-bin required without --check-only')
    out = args.build_dir.resolve(); binary = args.rust_test_bin.resolve()
    if out == ROOT or ROOT in out.parents:
        raise GateError('fresh external build directory required')
    rust_bytes = bounded(binary, 256 << 20); out.mkdir(parents=True, exist_ok=False)
    (out / 'source-pins.json').write_text(json.dumps(dict(inputs={p.as_posix(): sha(b) for p, b in data.items()},
        approved_product_pins=APPROVED_PINS, rust_test_binary=str(binary), rust_test_sha256=sha(rust_bytes)), indent=2) + '\n')
    results = []
    try:
        rc, listing = run([str(binary), '--list', '--ignored'], out, 'inventory', 30)
        if rc or set(re.findall(r'^(\w+): test$', listing, re.MULTILINE)) != RUST_TESTS:
            raise GateError('prebuilt Rust binary must contain exactly the five reviewed D6 tests')
        directory = out / 'baseline'; tree = materialize(directory, data); fixture = build(directory, tree, args.cc)
        native_results = []
        for case in NATIVE_CASES:
            evidence = directory / case; evidence.mkdir()
            rc, _ = native(fixture, evidence, case); native_results.append(dict(case=case, rc=rc))
            (directory / 'native-results.json').write_text(json.dumps(native_results, indent=2) + '\n')
            if rc:
                raise GateError(f'unchanged native baseline failed: {case}; no mutation evidence valid')
        evidence = directory / 'rust'; evidence.mkdir(); rc, text = rust(binary, fixture, evidence)
        if rc or 'test result: ok. 5 passed; 0 failed; 0 ignored;' not in text:
            raise GateError('unchanged Rust5 baseline failed; no mutation evidence valid')
        print('BASELINE native8/8 and Rust5/5 passed', flush=True)
        for v in changes:
            directory = out / v['name']; tree = materialize(directory, data, v); fixture = build(directory, tree, args.cc)
            evidence = directory / 'witness'; evidence.mkdir(); witness = v['witness']
            rc, text = (native(fixture, evidence, witness['case']) if witness['mode'] == 'native'
                        else rust(binary, fixture, evidence, witness['case']))
            record = dict(name=v['name'], sources={p.as_posix(): sha(b) for p, b in v['sources'].items()},
                          witness=witness, rc=rc, killed=killed(rc, text, witness))
            results.append(record); (directory / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
            if not record['killed']:
                raise GateError(f'{v["name"]}: designated semantic witness absent; inspect retained evidence')
            print(f'KILLED {v["name"]} by {witness["marker"]}', flush=True)
    finally:
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        if inventory() != sorted(data) or any(bounded(ROOT / p) != b for p, b in data.items()) or bounded(binary, 256 << 20) != rust_bytes:
            raise GateError('live checkout or Rust test binary changed during isolated batch')
    print('PASS 8 D6 lifecycle/selector controls; original checkout unchanged')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (GateError, subprocess.TimeoutExpired, OSError, ValueError) as error:
        print(f'FAIL HARNESS: {error}', file=sys.stderr)
        raise SystemExit(2)
