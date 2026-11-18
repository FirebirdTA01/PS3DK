#!/usr/bin/env python3
"""Build real D5 runtime witnesses; caller owns the host build slot.

--red-stubs links a test-only rejecting API while core wiring is absent.
Ordinary/GREEN builds must omit it. Real output validation is the explicit
ignored Rust rolling_runtime_ppu target using the three printed executable paths.
"""
import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--red-stubs', action='store_true')
    parser.add_argument('--sanitize', choices=('address', 'undefined'))
    parser.add_argument('--run-native', action='store_true', help='also execute native assertions; Rust artifact validation is still required')
    args = parser.parse_args()
    out = args.build_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    lib = ROOT / 'sdk/libps3dkprof/src'
    include = out / 'include/ps3dk'; include.mkdir(parents=True)
    shutil.copyfile(ROOT / 'sdk/include/ps3dk/prof.h', include / 'prof.h')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
             '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS',
             '-I', str(include.parent), '-I', str(lib / 'core'), '-I', str(lib / 'rolling')]
    if args.sanitize:
        flags += ['-fsanitize=' + ('address,undefined' if args.sanitize == 'address' else 'undefined'),
                  '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-ftls-model=local-exec']
    files = [str(lib / p) for p in ('core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c',
             'pal/pal_host.c', 'rolling/roll_store.c', 'rolling/roll_file.c', 'rolling/roll_output.c',
             'rolling/roll_ppu.c', 'rolling/roll_retention_inspect.c')]
    files += [str(HERE / 'runtime_ppu_test.c')]
    if args.red_stubs:
        files += [str(HERE / 'runtime_ppu_red.c')]
    else:
        files += [str(lib / 'rolling/roll_runtime.c')]
    wraps = ['-Wl,--wrap=ps3dk_prof__pal_' + name for name in
             ('open_excl', 'open_rw_excl', 'close', 'alloc', 'thread_create')]
    wraps += ['-Wl,--wrap=ps3dk_prof__roll_ppu_init', '-Wl,--wrap=ps3dk_prof__roll_output_accept']
    builds = []
    for name, extra in [('runtime-ppu', ['-DPS3DK_PROF_ROLLING']), ('runtime-ppu-gate-off', []),
                        ('runtime-ppu-arcs', ['-DPS3DK_PROF_ROLLING', '-DPS3DK_PROF_WITH_ARCS=1',
                         str(lib / 'hooks/arcs.c'), str(lib / 'core/prof_arcs_writer.c')])]:
        exe = out / name
        command = shlex.split(args.cc) + flags + extra + files + wraps + ['-o', str(exe)]
        with (out / (name + '.build.stdout')).open('xb') as stdout, (out / (name + '.build.stderr')).open('xb') as stderr:
            got = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=180)
        builds.append(dict(name=name, command=command, rc=got.returncode))
        (out / 'builds.json').write_text(json.dumps(builds, indent=2) + '\n')
        if got.returncode:
            raise RuntimeError(f'{name}: compiler failed ({got.returncode}); see retained build logs')
    print(f'PS3DK_PROF_RUNTIME_PPU={out / "runtime-ppu"}', flush=True)
    print(f'PS3DK_PROF_RUNTIME_PPU_GATE_OFF={out / "runtime-ppu-gate-off"}', flush=True)
    print(f'PS3DK_PROF_RUNTIME_PPU_ARCS={out / "runtime-ppu-arcs"}', flush=True)
    if args.run_native:
        cases = [(name, 'runtime-ppu') for name in ('append', 'bootstrap', 'rotation', 'resource', 'resource-shutdown', 'worker-failure',
                 'invalid-cap', 'invalid-payload', 'invalid-checkpoint', 'invalid-state', 'invalid-overflow', 'invalid-callback')]
        cases += [('gate-off', 'runtime-ppu-gate-off'), ('unsupported-arcs', 'runtime-ppu-arcs')]
        results = []
        for name, binary in cases:
            path = out / name; path.mkdir()
            with (path / 'stdout').open('xb') as stdout, (path / 'stderr').open('xb') as stderr:
                got = subprocess.run([str(out / binary), name, str(path)], stdout=stdout, stderr=stderr, timeout=60)
            results.append(dict(case=name, rc=got.returncode))
            (out / 'native-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(f'{name}: rc {got.returncode}', flush=True)
        return int(any(r['rc'] for r in results))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
