#!/usr/bin/env python3
"""Snapshot all sample files, including ignored outputs, before/after builds.

--selftest runs isolated leak/change/delete controls without a compiler.
--output names a fresh directory and retains logs, snapshots and build outputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def snapshot(directory):
    result = {}
    for path in sorted(directory.rglob('*')):
        name = path.relative_to(directory).as_posix()
        if path.is_symlink():
            result[name] = ['symlink', os.readlink(path)]
        elif path.is_file():
            data = path.read_bytes()
            result[name] = [len(data), hashlib.sha256(data).hexdigest()]
    return result


def changes(before, after):
    return {'added': sorted(after.keys() - before.keys()),
            'removed': sorted(before.keys() - after.keys()),
            'modified': sorted(k for k in before.keys() & after.keys() if before[k] != after[k])}


def save(path, value):
    path.write_text(json.dumps(value, indent=2), newline='\n')


def sdk_roots(ps3dev, ps3dk=None):
    dev = Path(ps3dev).resolve()
    if ps3dk:
        return dev, Path(ps3dk).resolve()
    nested = dev / 'ps3dk'
    return dev, nested if nested.is_dir() else dev


def controls(out):
    fixture = out / 'controls'
    fixture.mkdir()
    preexisting = fixture / 'preexisting.fake.self'
    original = b'preexisting user artifact\n'
    preexisting.write_bytes(original)
    baseline = snapshot(fixture)
    rows = []

    def check(ok, name):
        rows.append({'name': name, 'pass': bool(ok)})
        save(out / 'controls.json', rows)
        if not ok:
            raise AssertionError(name)

    check('preexisting.fake.self' in baseline, 'preexisting ignored file is tracked')
    check(snapshot(fixture) == baseline, 'clean GREEN')
    for suffix in ('fake.self', 'fake.sprx', 'o', 'a'):
        leak = fixture / ('leak.' + suffix)
        with leak.open('xb') as stream:
            stream.write(b'generated artifact\n')
        check(changes(baseline, snapshot(fixture))['added'] == [leak.name], 'leak RED ' + suffix)
        leak.unlink()  # Only our exclusively created isolated fixture file.
        check(snapshot(fixture) == baseline, 'restored GREEN ' + suffix)
    preexisting.write_bytes(original.replace(b'preexisting', b'PREEXISTING'))
    check(changes(baseline, snapshot(fixture))['modified'] == [preexisting.name], 'modified RED')
    preexisting.unlink()
    check(changes(baseline, snapshot(fixture))['removed'] == [preexisting.name], 'removed RED')
    preexisting.write_bytes(original)
    check(snapshot(fixture) == baseline and preexisting.read_bytes() == original, 'preserved GREEN')
    dev = out / 'compiler-root'
    dev.mkdir()
    check(sdk_roots(dev) == (dev, dev), 'flat SDK root')
    (dev / 'ps3dk').mkdir()
    check(sdk_roots(dev) == (dev, dev / 'ps3dk'), 'split SDK root')
    separate = out / 'runtime-root'
    check(sdk_roots(dev, separate) == (dev, separate), 'explicit SDK root wins')
    print(f'samples-clean-build controls: {len(rows)} pass, 0 fail', flush=True)


def run(args, out):
    controls(out)
    if args.selftest:
        return
    if not args.ps3dev:
        print('samples-clean-build: SKIP builds (set PS3DEV or --ps3dev); controls passed')
        return
    dev, sdk = sdk_roots(args.ps3dev, args.ps3dk)
    cc = dev / 'ppu/bin' / ('powerpc64-ps3-elf-gcc.exe' if os.name == 'nt' else 'powerpc64-ps3-elf-gcc')
    if not cc.is_file():
        raise RuntimeError(f'configured SDK has no compiler: {cc}')
    repo = Path(__file__).resolve().parents[2]
    env = dict(os.environ, PS3DEV=str(dev), PS3DK=str(sdk))
    before = snapshot(repo / 'samples')
    save(out / 'before.json', before)
    generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
    builds = []
    try:
        for sample in ('sysutil/hello-ppu-png', 'vision/hello-ppu-camera'):
            for abi in ('ilp32', 'lp64'):
                name = sample.replace('/', '_') + '-' + abi
                build = out / name
                tc = repo / 'cmake' / ('ps3-ppu-toolchain-lp64.cmake' if abi == 'lp64' else 'ps3-ppu-toolchain.cmake')
                commands = [
                    ['cmake', '-S', str(repo / 'samples' / sample), '-B', str(build), *generator,
                     '-DCMAKE_TOOLCHAIN_FILE=' + tc.as_posix(), '-DPS3DEV=' + dev.as_posix(),
                     '-DPS3DK=' + sdk.as_posix()],
                    ['cmake', '--build', str(build), '--parallel', '2'],
                ]
                for phase, cmd in zip(('configure', 'build'), commands):
                    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    (out / (name + '-' + phase + '.log')).write_text(r.stdout, newline='\n')
                    builds.append({'name': name, 'phase': phase, 'command': cmd, 'rc': r.returncode})
                    if r.returncode:
                        raise RuntimeError(f'{name} {phase} failed; see log in {out}')
                self_file = build / (Path(sample).name + '.fake.self')
                if not self_file.is_file():
                    raise RuntimeError(f'missing binary-directory SELF: {self_file}')
                if sample.endswith('png') and not (build / 'pkg_files/USRDIR/ps3dk.png').is_file():
                    raise RuntimeError('PNG boot asset is not beside the SELF')
                print(f'samples-clean-build: {name} PASS', flush=True)
    finally:
        after = snapshot(repo / 'samples')
        save(out / 'after.json', after)
        delta = changes(before, after)
        save(out / 'changes.json', delta)
        save(out / 'commands.json', builds)
        if any(delta.values()):
            raise RuntimeError(f'sample source changed: {delta}')
    print('samples-clean-build: 4 builds pass, 0 fail; source snapshot unchanged')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps3dev', default=os.environ.get('PS3DEV') or os.environ.get('PS3DK'))
    parser.add_argument('--ps3dk', default=os.environ.get('PS3DK'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    try:
        if args.output:
            args.output = args.output.resolve()
            args.output.mkdir(parents=True, exist_ok=False)
            run(args, args.output)
        else:
            build_root = Path(os.environ.get('PS3_BUILD_ROOT', Path.home() / 'ps3tc/build'))
            build_root.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='samples-clean-', dir=build_root) as temp:
                run(args, Path(temp))
        return 0
    except (OSError, RuntimeError, AssertionError) as error:
        print(f'samples-clean-build: FAIL: {error}', flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
