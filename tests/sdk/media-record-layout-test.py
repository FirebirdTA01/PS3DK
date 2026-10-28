#!/usr/bin/env python3
"""Compile ABI assertions with the actual PPU compiler; never install headers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ps3dev', default=os.environ.get('PS3DEV'))
    parser.add_argument('--include', type=Path, default=root / 'sdk/include')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--require-toolchain', action='store_true')
    args = parser.parse_args()
    if not args.ps3dev:
        print('media-record-layout: SKIP (set PS3DEV or --ps3dev)')
        return int(args.require_toolchain)
    cc = Path(args.ps3dev) / 'ppu/bin/powerpc64-ps3-elf-gcc'
    if not cc.is_file():
        cc = cc.with_suffix('.exe')
    if not cc.is_file():
        parser.error('PPU compiler missing')
    target = subprocess.run([str(cc), '-dumpmachine'], text=True,
                            capture_output=True, timeout=30, check=True)
    if target.stdout.strip() != 'powerpc64-ps3-elf':
        parser.error('not a PS3 PPU compiler')
    work = args.work_dir
    if work is None:
        with tempfile.TemporaryDirectory(prefix='media-layout-') as temp:
            return matrix(cc, args.include, Path(temp), root)
    work.mkdir(parents=True, exist_ok=False)
    return matrix(cc, args.include, work, root)


def matrix(cc, inc, work, root):
    src = root / 'tests/sdk/media-record-layout-test.c'
    fails = 0
    for group, name in enumerate(['voice', 'dbgfont', 'search', 'sail']):
        for lang, std in [('c', 'gnu99'), ('c', 'c11'), ('c++', 'c++17')]:
            for abi in ['ilp32', 'lp64']:
                label = f'{name}-{std}-{abi}'
                cmd = [str(cc), '-x', lang, '-std=' + std, '-O2', '-Wall',
                       '-Wextra', '-Werror', '-DMEDIA_GROUP=' + str(group),
                       '-I' + str(inc)]
                if abi == 'lp64':
                    cmd.append('-mlp64')
                cmd += ['-c', str(src), '-o', str(work / (label + '.o'))]
                result = subprocess.run(cmd, capture_output=True, text=True,
                                        errors='replace', timeout=60)
                (work / (label + '.log')).write_text(
                    repr(cmd) + '\n' + result.stdout + result.stderr,
                    encoding='utf-8')
                print(f'media-record-layout: {"FAIL" if result.returncode else "PASS"} {label}')
                fails += bool(result.returncode)
    print(f'media-record-layout: {24 - fails}/24 PASS')
    return int(fails != 0)


if __name__ == '__main__':
    raise SystemExit(main())
