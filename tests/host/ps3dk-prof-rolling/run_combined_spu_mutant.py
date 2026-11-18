#!/usr/bin/env python3
"""Isolated typed-caller-validation sensitivity; compiler ownership is external.

The unchanged named Rust witness first passes with original C. The mutant is
killed only by its expected original-position INVALID assertion, never by a
compile failure, crash, timeout, or unrelated fixture failure. --check-only
reads sources and checks the unique mutation anchor without running anything.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SRC = ROOT / 'sdk/libps3dkprof/src/rolling'
TEST = 'mixed_spu_and_rsx_domain_is_refused_whole_under_schema1'
ANCHOR = 'rc=sp_accept(spu(c,b),&c->spu_layout,&c->cfg,&v,ty,raw+16,len,flags);'
PINS = {
    # Reviewed shared-core extraction: same typed caller anchor now lives in
    # core_accept; schema1 passes no external policy and zero family flags.
    # Gate97 ASan schema1/SPU/resource/physical and mixed39 parity all passed.
    'roll_combined.c': '63824d372ae119055b55f00e84524b312c04ab7baebf2e6f8fef4882f0b7c5ff',
    'roll_combined.h': '0990f047c5bfd0575e5f627e0a0729ebed325fca16ba61a76140427558812431',
    'roll_combined_internal.h': 'e455d68426dc4bed3e1e5ab7f052a975265715c18a1b06ae0de39d3bec644b05',
    'roll_combined_spu.h': 'c9bb915cf8eb08b059ef8e92532284375b7f33eb4b83a7c16b28f9748670212a',
    'roll_ppu.c': '0e300f2a7b09ea2c25abca085f5c0f06b52abafda78a0df8546d814c5d2478da',
    'roll_ppu_internal.h': '4faa531a02488930fb7647df6b829c3e1403a9e50a21468821a8cf0aa7c2d929',
}


def digest(b):
    return hashlib.sha256(b).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--rust-test', type=Path)
    p.add_argument('--cc', default='cc')
    p.add_argument('--check-only', action='store_true')
    a = p.parse_args()
    files = list(SRC.glob('roll_combined*.h')) + [SRC / n for n in
        ['roll_combined.c', 'roll_ppu.c', 'roll_ppu.h', 'roll_ppu_internal.h', 'roll_output.h', 'roll_store.h']]
    files += [HERE / n for n in ['combined_spu_encoder_fixture.c', 'combined_encoder_fixture.c', 'combined_encoder_config.h']]
    files += [ROOT / 'tools/ps3dk-prof/tests/rolling_combined_c_spu.rs', Path(__file__).resolve()]
    data = {}
    for f in files:
        if not f.is_file() or f.is_symlink() or f.stat().st_size > 1048576:
            raise RuntimeError(f'unbounded/nonregular source {f}')
        data[f] = f.read_bytes().replace(b'\r\n', b'\n')
    for name, expected in PINS.items():
        if digest(data[SRC / name]) != expected:
            raise RuntimeError(f'review source pin before mutation: {name}')
    original = data[SRC / 'roll_combined.c'].decode()
    if original.count(ANCHOR) != 1:
        raise RuntimeError('mutation anchor is not unique')
    if a.check_only:
        print('PASS caller-validation source pins and unique anchor (no builds)')
        return 0
    if not a.build_dir or not a.rust_test:
        p.error('--build-dir and --rust-test are required for execution')
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    rust = a.rust_test.resolve(strict=True)
    records = {'sources': {str(f.relative_to(ROOT)): digest(b) for f, b in data.items()},
               'rust_binary': str(rust), 'rust_sha256': digest(rust.read_bytes()), 'test': TEST, 'runs': []}
    for name, body in [('baseline', original), ('bypass_typed_caller_invalid', original.replace(
            ANCHOR, ANCHOR + '\n        if(rc==PFR_INVALID)rc=PFR_OK;'))]:
        work = out / name
        work.mkdir()
        for f, b in data.items():
            if f.parent == SRC or f.parent == HERE:
                (work / f.name).write_bytes(b)
        (work / 'roll_combined.c').write_text(body, encoding='utf-8', newline='\n')
        exe = work / ('encoder.exe' if sys.platform == 'win32' else 'encoder')
        command = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic',
            '-I', str(work), str(work / 'combined_spu_encoder_fixture.c'), str(work / 'roll_combined.c'),
            str(work / 'roll_ppu.c'), '-o', str(exe)]
        build = subprocess.run(command, capture_output=True, timeout=180)
        (work / 'compile.log').write_bytes(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError(f'{name}: compile failure is not a mutant kill')
        env = dict(os.environ, PS3DK_PROF_ROLLING_COMBINED_SPU_ENCODER=str(exe))
        result = subprocess.run([str(rust), '--ignored', '--exact', TEST, '--nocapture'], env=env, capture_output=True, timeout=180)
        log = result.stdout + result.stderr
        (work / 'test.log').write_bytes(log)
        if name == 'baseline':
            if result.returncode != 0 or b'1 passed; 0 failed' not in log:
                raise RuntimeError('unchanged named witness did not pass')
        elif result.returncode != 101 or b'expected INVALID at original' not in log or b'0 passed; 1 failed' not in log:
            raise RuntimeError('mutant did not fail the named semantic assertion')
        records['runs'].append({'name': name, 'rc': result.returncode, 'log_sha256': digest(log)})
    for f, b in data.items():
        if f.read_bytes().replace(b'\r\n', b'\n') != b:
            raise RuntimeError(f'source changed during gate: {f}')
    (out / 'results.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    print('PASS baseline and caller-validation mutant killed by named original-position assertion')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
