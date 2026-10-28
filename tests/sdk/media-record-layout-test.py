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
                       '-Wextra', '-Werror', '-DEXPECTED_ABI_BYTES=' + ('8' if abi == 'lp64' else '4'),
                       '-DMEDIA_GROUP=' + str(group),
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
    if fails:
        return 1
    return controls(cc, inc, work, root)


def controls(cc, inc, work, root):
    # Each defect changes one independent ABI property and must trigger its
    # named assertion, rather than merely causing an unrelated compiler error.
    cases = [
        ('voice-size', 0, 'cell/voiceTypes.h',
         '} CellVoiceBasePortInfo __attribute__((aligned(64)));',
         '} __attribute__((aligned(64))) CellVoiceBasePortInfo;',
         'CellVoiceBasePortInfo size'),
        ('font-option', 1, 'cell/dbgfont.h',
         'uint32_t   option;', 'uint64_t   option;',
         'CellDbgFontConfigGcm size'),
        ('font-columns', 1, 'cell/dbgfont.h',
         'uint32_t cnsWidth;', 'uint16_t cnsWidth;',
         'CellDbgFontConsoleConfig.cnsWidth width'),
        ('search-time', 2, 'cell/sysutil_search_types.h',
         'int64_t    takenDate;', 'int32_t    takenDate;',
         'CellSearchTimeInfo.takenDate width'),
        ('sail-start', 3, 'cell/sail/common.h',
         'uint32_t startType;', 'uint64_t startType;',
         'CellSailStartCommand.startType width'),
    ]
    failures = 0
    for name, group, relative, old, new, diagnostic in cases:
        original = (inc / relative).read_text(encoding='utf-8')
        if original.count(old) != 1:
            raise RuntimeError('mutation anchor is not unique: ' + name)
        overlay = work / ('mutant-' + name)
        header = overlay / relative
        header.parent.mkdir(parents=True)
        header.write_text(original.replace(old, new), encoding='utf-8')
        for abi in ['ilp32', 'lp64']:
            label = name + '-' + abi
            cmd = [str(cc), '-x', 'c', '-std=c11', '-O2', '-Wall',
                   '-Wextra', '-Werror', '-DEXPECTED_ABI_BYTES=' + ('8' if abi == 'lp64' else '4'),
                       '-DMEDIA_GROUP=' + str(group),
                   '-I' + str(overlay), '-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += ['-c', str(root / 'tests/sdk/media-record-layout-test.c'),
                    '-o', str(work / (label + '.o'))]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            output = result.stdout + result.stderr
            (work / (label + '.log')).write_text(repr(cmd) + '\n' + output,
                                               encoding='utf-8')
            caught = result.returncode != 0 and diagnostic in output
            print('media-record-layout: mutant ' + label +
                  (' caught' if caught else ' NOT CAUGHT'))
            failures += not caught
    return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
