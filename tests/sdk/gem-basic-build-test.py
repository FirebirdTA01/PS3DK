#!/usr/bin/env python3
"""Build both Move sample ABIs, retain outputs, check imports and source cleanliness."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps3dev', default=os.environ.get('PS3DEV'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--ninja', default=shutil.which('ninja'))
    args = parser.parse_args()
    if not args.ps3dev:
        print('gem-basic-build: SKIP (PS3DEV not set)')
        return 0
    root = Path(__file__).resolve().parents[2]
    sdk = Path(args.ps3dev).resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    compiler = sdk / 'ppu/bin' / ('powerpc64-ps3-elf-g++' + suffix)
    nm = sdk / 'ppu/bin' / ('powerpc64-ps3-elf-nm' + suffix)
    if not compiler.is_file() or not nm.is_file():
        raise RuntimeError('Explicit SDK lacks PPU compiler/nm')
    if args.output:
        out = args.output.resolve()
        out.mkdir(parents=True, exist_ok=True)
        if any(out.iterdir()):
            raise RuntimeError('Output must be empty; previous evidence is retained')
    else:
        build_root = Path(os.environ.get('PS3_BUILD_ROOT', Path.home() / 'ps3tc/build'))
        build_root.mkdir(parents=True, exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix='gem-basic-', dir=build_root))
    env = dict(os.environ, PS3DEV=str(sdk), PS3DK=str(sdk), PSL1GHT=str(sdk))
    env['PATH'] = str(sdk / 'bin') + os.pathsep + env['PATH']
    commands = []

    def run(name, argv):
        argv = [str(x) for x in argv]
        result = subprocess.run(argv, cwd=root, env=env, capture_output=True, text=True)
        (out / (name + '.stdout')).write_text(result.stdout, encoding='utf-8', newline='\n')
        (out / (name + '.stderr')).write_text(result.stderr, encoding='utf-8', newline='\n')
        commands.append(dict(name=name, argv=argv, rc=result.returncode))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8', newline='\n')
        if result.returncode:
            raise RuntimeError(f'{name} rc={result.returncode}: {result.stdout}\n{result.stderr}')
        return result.stdout

    before = snapshot(root / 'samples')
    tools = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (compiler, nm)}
    (out / 'tools.json').write_text(json.dumps(tools, indent=2), encoding='utf-8', newline='\n')
    run('compiler-version', [compiler, '-v'])
    includes = out / 'headers.cmake'
    includes.write_text('include_directories(BEFORE "' + (root / 'sdk/include').as_posix() + '")\n',
                        encoding='utf-8', newline='\n')
    required = ('cellGemInit', 'cellGemEnd', 'cellGemGetInfo', 'cellGemGetInertialState',
                'cellGemSetRumble', 'cellGemGetRumble', 'cellGemForceRGB', 'cellGemGetRGB',
                'cellSpursInitializeWithAttribute', 'cellSpursFinalize')
    passed = 0
    try:
        for abi, toolchain in [('ilp32', 'ps3-ppu-toolchain.cmake'), ('lp64', 'ps3-ppu-toolchain-lp64.cmake')]:
            dest = out / abi
            configure = ['cmake', '-S', root / 'samples/input/hello-ppu-gem-basic', '-B', dest,
                         '--toolchain', root / 'cmake' / toolchain, '-DCMAKE_BUILD_TYPE=Release',
                         '-DCMAKE_PROJECT_INCLUDE=' + includes.as_posix()]
            if args.ninja:
                configure += ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + args.ninja]
            run(abi + '-configure', configure)
            run(abi + '-build', ['cmake', '--build', dest, '--parallel', '2'])
            passed += 1
            for extension in ('.elf', '.self', '.fake.self'):
                artifact = dest / ('hello-ppu-gem-basic' + extension)
                assert artifact.is_file() and artifact.stat().st_size > 0, artifact
            passed += 1
            # The SDK archive groups all GEM stubs in one member. Its linked
            # export set includes unused update stubs; inspect actual caller
            # objects for references, while retaining the linked symbol table.
            run(abi + '-symbols', [nm, '-a', dest / 'hello-ppu-gem-basic'])
            objects = sorted((dest / 'CMakeFiles/hello-ppu-gem-basic.dir').rglob('*.o'))
            objects += sorted((dest / 'CMakeFiles/hello-ppu-gem-basic.dir').rglob('*.obj'))
            assert len(objects) == 2, objects
            symbols = run(abi + '-caller-imports', [nm, '-u', *objects])
            for symbol in required:
                assert symbol in symbols, f'{abi}: missing {symbol}'
            for forbidden in ('cellCamera', 'cellGemUpdate', 'cellGemGetState', 'cellGemConvertVideo'):
                assert forbidden not in symbols, f'{abi}: camera/pose import {forbidden}'
            passed += 1
        assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == h for p, h in tools.items())
    finally:
        after = snapshot(root / 'samples')
        (out / 'source-before.json').write_text(json.dumps(before, indent=2), encoding='utf-8', newline='\n')
        (out / 'source-after.json').write_text(json.dumps(after, indent=2), encoding='utf-8', newline='\n')
        assert before == after, 'Build changed sample sources (including ignored outputs)'
    passed += 1
    print(f'gem-basic-build: {passed} checks, {passed} pass, 0 fail; outputs {out}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
