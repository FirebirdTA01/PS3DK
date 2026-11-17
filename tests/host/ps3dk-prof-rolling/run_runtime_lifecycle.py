#!/usr/bin/env python3
"""Build real private rolling lifecycle controls; caller owns the build slot.

Discover all compiler dependencies before compiling, record their SHA256 plus
the exact command, then refuse changed inputs or a changed dependency inventory.
This gate never edits runtime source or substitutes a lifecycle implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CASES = ['thread-auto', 'race-resource-first', 'race-public-first', 'init-format-write',
         'init-bootstrap-sync', 'io-tail-close', 'io-terminal-sync', 'close-join',
         'resource-close-join', 'resource-join', 'join-only', 'eligible-thread',
         'quick-normal', 'quick-busy', 'quick-resource']


def sha(path):
    if not path.is_file() or path.stat().st_size > 256 << 20:
        raise RuntimeError(f'not a bounded compiler input: {path}')
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(65536), b''):
            digest.update(block)
    return digest.hexdigest()


def run(command, out, label, timeout):
    with (out / (label + '.stdout')).open('xb') as stdout, (out / (label + '.stderr')).open('xb') as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout)
    return result.returncode


def candidates(lib):
    # Includes files that could become a newly added include between dependency
    # discovery and compilation; changes to that inventory also invalidate proof.
    paths = [p.resolve() for p in lib.rglob('*') if p.is_file() and p.suffix in ('.h', '.c')]
    paths += [ROOT / 'sdk/include/ps3dk/prof.h', HERE / 'runtime_lifecycle_test.c',
              HERE / 'runtime_ppu_test.c', Path(__file__).resolve()]
    return sorted(set(paths))


def dependencies(cc, flags, files, out, pass_name):
    found = set()
    for i, source in enumerate(files):
        label = f'{pass_name}-{i:02}'
        command = cc + flags + ['-M', '-MT', 'input', str(source)]
        if run(command, out, label, 60):
            raise RuntimeError(f'{label}: dependency discovery failed; not test evidence')
        path = out / (label + '.stdout')
        if path.stat().st_size > 1 << 20:
            raise RuntimeError('dependency output exceeds bounded receipt size')
        text = path.read_text().replace('\\\n', ' ')
        if not text.startswith('input:'):
            raise RuntimeError(f'{label}: unexpected compiler dependency format')
        found.update(Path(p).resolve() for p in shlex.split(text.split(':', 1)[1]))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', choices=('address', 'undefined', 'thread'))
    parser.add_argument('--run-native', action='store_true')
    args = parser.parse_args()
    out = args.build_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    lib = ROOT / 'sdk/libps3dkprof/src'
    include = out / 'include/ps3dk'; include.mkdir(parents=True)
    shutil.copyfile(ROOT / 'sdk/include/ps3dk/prof.h', include / 'prof.h')
    cc = shlex.split(args.cc)
    if not cc:
        raise RuntimeError('empty compiler command')
    compiler = shutil.which(cc[0])
    if compiler is None:
        raise RuntimeError('compiler executable unavailable')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
             '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DPS3DK_PROF_ROLLING',
             '-I', str(include.parent), '-I', str(lib / 'core'), '-I', str(lib / 'rolling')]
    if args.sanitize:
        flags += ['-fsanitize=' + ('address,undefined' if args.sanitize == 'address' else args.sanitize),
                  '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-ftls-model=local-exec']
    files = [lib / p for p in ('core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c',
             'rolling/roll_runtime.c', 'rolling/roll_store.c', 'rolling/roll_file.c', 'rolling/roll_output.c',
             'rolling/roll_ppu.c', 'rolling/roll_retention_inspect.c')]
    files += [HERE / 'runtime_lifecycle_test.c']
    wrapped = ['ps3dk_prof__pal_' + name for name in ('open_excl', 'open_rw_excl', 'close', 'alloc', 'thread_create',
               'mutex_lock', 'mutex_unlock', 'mutex_trylock', 'retain_complete_kind', 'sleep_us', 'thread_join')]
    wrapped += ['ps3dk_prof__roll_ppu_init', 'ps3dk_prof__roll_output_accept', 'ps3dk_prof__roll_runtime_flush',
                'ps3dk_prof__roll_publish_state', 'pwrite', 'pwrite64', 'fsync']
    exe = out / 'runtime-lifecycle'
    command = cc + flags + [str(p) for p in files] + ['-Wl,--wrap=' + name for name in wrapped] + ['-o', str(exe)]
    inventory = candidates(lib); initial = {str(p): sha(p) for p in inventory}
    discovered_before = dependencies(cc, flags, files, out, 'deps-before')
    inputs = set(discovered_before)
    if candidates(lib) != inventory or any(sha(Path(p)) != digest for p, digest in initial.items()):
        raise RuntimeError('source changed during dependency discovery; refusing build')
    inputs.update(inventory); inputs.add(Path(compiler).resolve())
    pins = {str(p): sha(p) for p in sorted(inputs)}
    command_sha = hashlib.sha256(json.dumps(command, separators=(',', ':')).encode()).hexdigest()
    receipt = dict(command=command, flags=flags, command_sha256=command_sha, inputs=pins, executable=str(exe))
    (out / 'build-inputs-before.json').write_text(json.dumps(receipt, indent=2) + '\n')
    rc = run(command, out, 'compile', 180)
    stable = candidates(lib) == inventory and all(sha(Path(p)) == digest for p, digest in pins.items())
    receipt.update(compile_rc=rc, input_hashes_unchanged=stable)
    if not rc:
        receipt['executable_sha256'] = sha(exe)
    (out / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if not stable:
        raise RuntimeError('compiler input changed during build; executable is not valid evidence')
    if rc:
        raise RuntimeError(f'compiler failed ({rc}); see compile.stderr')
    after = dependencies(cc, flags, files, out, 'deps-after')
    # Compare exact compiler-discovered sets, independently of the wider guard inventory.
    receipt['dependency_inventory_unchanged'] = after == discovered_before
    receipt['input_hashes_unchanged'] = candidates(lib) == inventory and all(sha(Path(p)) == digest for p, digest in pins.items())
    (out / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if not receipt['dependency_inventory_unchanged'] or not receipt['input_hashes_unchanged']:
        raise RuntimeError('inputs/dependencies changed across compilation; refusing execution')
    print(f'PS3DK_PROF_RUNTIME_LIFECYCLE={exe}', flush=True)
    if args.run_native:
        results = []
        for name in CASES:
            path = out / name; path.mkdir()
            rc = run([str(exe), name, str(path)], path, 'native', 60)
            results.append(dict(case=name, rc=rc))
            (out / 'native-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(f'{name}: rc {rc}', flush=True)
        if any(r['rc'] for r in results):
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
