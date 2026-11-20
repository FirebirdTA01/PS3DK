#!/usr/bin/env python3
"""Named shared-core extraction sensitivity; caller owns all builds.

Baseline witnesses must pass first. Mutants count only when the named Rust/C
semantic assertion fails; compiler errors, crashes, timeouts and sanitizer
failures are never kills. Source copies and result receipts stay under a fresh
external build directory. --check-only reads pins/anchors without subprocesses.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

from run_combined_spu_mutant import PINS as SHARED_PINS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SRC = ROOT / 'sdk/libps3dkprof/src/rolling'
PINS = dict(SHARED_PINS, **{
    'roll_combined2.c': 'd690a550814c25cc6d46c4eece72453330ce2777b89fe5fb1ac3aa27da3f69ac',
    'roll_combined2.h': '5cc46860e697320289fd9b864b0dbb33b609711a588db9c8b1a0841a8f05ca8e',
    'roll_combined2_auto.h': '40f336f61a87dffd1d1d01971d07c799c55d31d48afd8450d3728aa982370c64',
    'roll_combined2_fifo.h': '49e2976f7a04f6eb4224825014eeb823da43fc764473c67fc799fecc685a4bb8',
})
ORDINAL = 'c_fifo_schema2_interleaved_spu_status_replacement_preserves_original_support_ordinals'
CAPACITY = 'c_fifo_schema2_latest_status_uses_net_metadata_capacity'
BUDGET = 'c_fifo_schema2_core_and_family_share_one_checked_work_allowance'
SCHEMA1 = 'mixed_spu_and_rsx_domain_is_refused_whole_under_schema1'


def digest(b):
    return hashlib.sha256(b).hexdigest()


def specs():
    return [
        ('stale_spu_support_ordinal', ORDINAL, 'fifo', 'C canonical schema2 at original cut', [
            ('roll_combined_spu.h', 'w32(out+at+24,e->kind==1?current:e->count);',
             'if(e->kind==1)current=e->ordinal;\n        w32(out+at+24,e->kind==1?current:e->count);')]),
        ('replace_accepted_status', CAPACITY, 'metadata', 'PFM2_META_ASSERT[replacement-accepted-immutable]', [
            ('roll_ppu.c', 'if(old_bytes){memmove(metadata+old_offset,',
             'if(old_bytes){metadata=(uint8_t *)(void *)accepted+c->layout.metadata_offset;memmove(metadata+old_offset,')]),
        ('charge_transient_status_bytes', CAPACITY, 'metadata', 'PFM2_META_ASSERT[exact-net-capacity]', [
            ('roll_ppu.c', 'retained-=old_bytes;', 'retained-=0;')]),
        ('omit_metadata_cap_admission', CAPACITY, 'metadata', 'PFM2_META_ASSERT[short-or-invalid-boundary]', [
            ('roll_ppu.c', 'if(n>c->cfg.metadata_bytes||retained>c->cfg.metadata_bytes-n||4>c->cfg.metadata_bytes-n-retained){',
             'if(0){')]),
        ('schema1_enables_rsx_mode', SCHEMA1, 'spu', 'expected INVALID at original', [
            ('roll_combined.c', 'sp_plan(cfg,rsx,family!=0,&c->spu_layout)', 'sp_plan(cfg,64,1,&c->spu_layout)')]),
        ('independent_core_family_budgets', BUDGET, 'fifo', 'PFM2_ASSERT[shared-work-sum-and-immutable-refusal]', [
            ('roll_combined2.c', 'core_work>c->cfg.shared.prepare_work-family_work', 'core_work>c->cfg.shared.prepare_work'),
            ('roll_combined2.c', 'policy,c->cfg.shared.prepare_work-family_work,result)',
             'policy,c->cfg.shared.prepare_work,result)')]),
    ]


def snapshot():
    names = set(PINS) | {'roll_ppu.h', 'roll_output.h', 'roll_store.h'}
    files = [SRC / n for n in names]
    files += [HERE / n for n in ['combined2_encoder_config.h', 'combined2_encoder_fixture.c',
        'combined2_metadata_fixture.c', 'combined_spu_encoder_fixture.c', 'combined_encoder_fixture.c',
        'combined_encoder_config.h', 'run_combined_spu_mutant.py', Path(__file__).name]]
    tests = ROOT / 'tools/ps3dk-prof/tests'
    files += [tests / n for n in ['rolling_combined_c_fifo.rs', 'rolling_combined_c_spu.rs',
        'common/mod.rs', 'fifo_support/mod.rs', 'spu_support/mod.rs']]
    data = {}
    for f in files:
        if not f.is_file() or f.is_symlink() or f.stat().st_size > 2097152:
            raise RuntimeError(f'not a bounded regular source: {f}')
        data[f] = f.read_bytes().replace(b'\r\n', b'\n')
    for name, expected in PINS.items():
        if digest(data[SRC / name]) != expected:
            raise RuntimeError(f'review source pin before mutation: {name}')
    for name, _test, _kind, _marker, edits in specs():
        for path, old, _new in edits:
            if data[SRC / path].decode().count(old) != 1:
                raise RuntimeError(f'{name}: unique anchor changed in {path}')
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--fifo-test', type=Path)
    p.add_argument('--spu-test', type=Path)
    p.add_argument('--cc', default='cc')
    p.add_argument('--check-only', action='store_true')
    a = p.parse_args()
    data = snapshot()
    if a.check_only:
        print('PASS six shared-core mutation specifications, reviewed pins and anchors (no builds)')
        return 0
    if not a.build_dir or not a.fifo_test or not a.spu_test:
        p.error('--build-dir --fifo-test --spu-test are required')
    out = a.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        raise RuntimeError('external build directory required')
    out.mkdir(parents=True, exist_ok=False)
    binaries = {'fifo': a.fifo_test.resolve(strict=True), 'metadata': a.fifo_test.resolve(strict=True),
                'spu': a.spu_test.resolve(strict=True)}
    receipt = {'sources': {str(f.relative_to(ROOT)): digest(b) for f, b in data.items()},
               'test_binaries': {k: {'path': str(v), 'sha256': digest(v.read_bytes())} for k, v in binaries.items()}, 'runs': []}

    def execute(name, test, kind, marker, edits):
        work = out / name
        work.mkdir()
        for f, b in data.items():
            if f.parent in (SRC, HERE):
                (work / f.name).write_bytes(b)
        for path, old, new in edits:
            f = work / path
            body = f.read_text()
            if body.count(old) != 1:
                raise RuntimeError(f'{name}: changing mutation anchor')
            f.write_text(body.replace(old, new), encoding='utf-8', newline='\n')
        fixture = {'fifo': 'combined2_encoder_fixture.c', 'metadata': 'combined2_metadata_fixture.c',
                   'spu': 'combined_spu_encoder_fixture.c'}[kind]
        sources = [work / fixture, work / 'roll_combined.c', work / 'roll_ppu.c']
        if kind != 'spu':
            sources += [work / 'roll_combined2.c']
        exe = work / ('fixture.exe' if sys.platform == 'win32' else 'fixture')
        command = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(work)]
        command += list(map(str, sources)) + ['-o', str(exe)]
        build = subprocess.run(command, capture_output=True, timeout=180)
        (work / 'compile.log').write_bytes(build.stdout + build.stderr)
        row = {'name': name, 'test': test, 'command': command, 'compile_rc': build.returncode}
        receipt['runs'].append(row)
        if build.returncode:
            raise RuntimeError(f'{name}: compiler failure is not sensitivity evidence')
        env_name = {'fifo': 'PS3DK_PROF_ROLLING_COMBINED2_ENCODER', 'metadata': 'PS3DK_PROF_ROLLING_COMBINED2_METADATA',
                    'spu': 'PS3DK_PROF_ROLLING_COMBINED_SPU_ENCODER'}[kind]
        result = subprocess.run([str(binaries[kind]), '--ignored', '--exact', test, '--nocapture'],
            env=dict(os.environ, **{env_name: str(exe)}), capture_output=True, timeout=180)
        log = result.stdout + result.stderr
        (work / 'test.log').write_bytes(log)
        row.update(rc=result.returncode, log_sha256=digest(log), executable_sha256=digest(exe.read_bytes()))
        if marker is None:
            if result.returncode or b'1 passed; 0 failed' not in log:
                raise RuntimeError(f'{name}: unchanged witness did not pass')
        elif result.returncode != 101 or marker.encode() not in log or b'0 passed; 1 failed' not in log:
            raise RuntimeError(f'{name}: named semantic assertion did not kill mutant')

    try:
        seen = set()
        for _name, test, kind, _marker, _edits in specs():
            if (test, kind) not in seen:
                execute('baseline-' + kind + '-' + str(len(seen)), test, kind, None, [])
                seen.add((test, kind))
        for name, test, kind, marker, edits in specs():
            execute(name, test, kind, marker, edits)
        if snapshot() != data:
            raise RuntimeError('source changed during isolated gate')
        receipt['all_named_mutants_killed'] = True
    finally:
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('PASS unchanged baselines and six named shared-core mutants')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
