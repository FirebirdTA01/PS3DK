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
    parser.add_argument('--ps3dev', default=os.environ.get('PS3DEV') or os.environ.get('PS3DK'))
    parser.add_argument('--include', type=Path, default=root / 'sdk/include')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--require-toolchain', action='store_true')
    args = parser.parse_args()
    if not args.ps3dev:
        print('sail-record-layout: SKIP (set PS3DEV or --ps3dev)')
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
        with tempfile.TemporaryDirectory(prefix='sail-layout-') as temp:
            return matrix(cc, args.include, Path(temp), root)
    work.mkdir(parents=True, exist_ok=False)
    return matrix(cc, args.include, work, root)


def matrix(cc, inc, work, root):
    fails = 0
    src = root / 'tests/sdk/sail-record-layout-test.c'
    for lang, std in [('c', 'gnu99'), ('c', 'c11'), ('c++', 'c++17')]:
        for abi in ['ilp32', 'lp64']:
            label = f'{std}-{abi}'
            cmd = [str(cc), '-x', lang, '-std=' + std, '-O2', '-Wall',
                   '-Wextra', '-Werror',
                   '-DEXPECTED_ABI_BYTES=' + ('8' if abi == 'lp64' else '4'),
                   '-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += [str(src), '-o', str(work / (label + '.elf'))]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            (work / (label + '.log')).write_text(
                repr(cmd) + '\n' + result.stdout + result.stderr,
                encoding='utf-8')
            print(f'sail-record-layout: {"FAIL" if result.returncode else "PASS"} {label}')
            fails += bool(result.returncode)
    print(f'sail-record-layout: {6 - fails}/6 PASS')
    if not fails:
        fails += mutants(cc, inc, work, src)
    return int(fails != 0)


def mutants(cc, inc, work, src):
    """Each old layout defect and witness corruption must fail its own diagnostic."""
    nl = chr(10)
    cases = [
        ('sail-player-size', 'cell/sail/player.h',
         'typedef struct { uint64_t internalData[128]; } CellSailPlayer;',
         'typedef struct { uint64_t internalData[256]; } CellSailPlayer;',
         'CellSailPlayer size', ['ilp32', 'lp64']),
        ('sail-recorder-size', 'cell/sail/recorder.h',
         'typedef struct __attribute__((aligned(128))) CellSailRecorder { uint64_t internalData[1024]; } CellSailRecorder;',
         'typedef struct { uint64_t internalData[128]; } CellSailRecorder;',
         'CellSailRecorder size', ['ilp32', 'lp64']),
        ('sail-composer-size', 'cell/sail/muxer.h',
         'typedef struct { uint64_t internalData[40]; } CellSailComposer;',
         'typedef struct { uint64_t internalData[32]; } CellSailComposer;',
         'CellSailComposer size', ['ilp32', 'lp64']),
        ('sail-video-format-size', 'cell/sail/common.h',
         '    int8_t coding;' + nl + '    int8_t scan;' + nl + '    int8_t bitsPerColor;',
         '    int8_t coding;' + nl + '    int8_t scan;' + nl + '    int8_t pad[8];' + nl + '    int8_t bitsPerColor;',
         'CellSailVideoFormat size', ['ilp32', 'lp64']),
        ('sail-composer-video-au-pts', 'cell/sail/muxer.h',
         '    int64_t pts[3];',
         '    int64_t pts;',
         'CellSailComposerVideoAuInfo.dts offset', ['ilp32', 'lp64']),
        ('sail-avi-stream-priority-width', 'cell/sail/avi.h',
         '    uint16_t priority;' + nl + '    uint16_t language;',
         '    uint32_t priority;' + nl + '    uint32_t language;',
         'CellSailAviStreamHeader size', ['ilp32', 'lp64']),
        ('sail-audio-frame-lp64-pointer', 'cell/sail/renderer_audio.h',
         'typedef struct CellSailAudioFrameInfo { void *pPcm ATTRIBUTE_PRXPTR; int status; uint64_t pts, reserved; } CellSailAudioFrameInfo;',
         'typedef struct CellSailAudioFrameInfo { void *pPcm; int status; uint64_t pts, reserved; } CellSailAudioFrameInfo;',
         'CellSailAudioFrameInfo size', ['lp64']),
        ('sail-sound-funcs-lp64-slot', 'cell/sail/sound_adapter.h',
         '    CellSailSoundAdapterFuncMakeup        pMakeup        ATTRIBUTE_PRXPTR;',
         '    CellSailSoundAdapterFuncMakeup        pMakeup;',
         'CellSailSoundAdapterFuncs size', ['lp64']),
        ('sail-video-param-thissize', 'cell/sail/profile.h',
         'typedef struct CellSailBsVideoParameter {' + nl + '    uint32_t thisSize;',
         'typedef struct CellSailBsVideoParameter {' + nl + '    size_t thisSize;',
         'CellSailBsVideoParameter.thisSize width', ['lp64']),
        ('witness-always-fail', 'test:',
         '    return 0;' + nl + '}',
         '    sail_witness_decode_mismatch();' + nl + '    return 0;' + nl + '}',
         'sail_witness_decode_mismatch', ['ilp32', 'lp64']),
        ('witness-corrupted-callback', 'test:',
         'sound_buf[3] = 0x04;',
         'sound_buf[3] = 0x99;',
         'sail_witness_decode_mismatch', ['ilp32', 'lp64']),
        ('witness-corrupted-control-word', 'test:',
         'video_param_buf[3] = 80;',
         'video_param_buf[3] = 99;',
         'sail_witness_decode_mismatch', ['ilp32', 'lp64']),
    ]
    fails = 0
    for label, target_file, old, new, diagnostic, abis in cases:
        if target_file == 'test:':
            text = src.read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'sail-record-layout: FAIL cannot plant {label}')
                fails += 1
                continue
            mutant_src = work / f'{label}.c'
            mutant_src.write_text(text.replace(old, new), encoding='utf-8')
            cur_src = mutant_src
            overlay_args = []
        else:
            text = (inc / target_file).read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'sail-record-layout: FAIL cannot plant {label}')
                fails += 1
                continue
            overlay = work / label
            out = overlay / target_file
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text.replace(old, new), encoding='utf-8')
            cur_src = src
            overlay_args = ['-I' + str(overlay)]
        for abi in abis:
            cmd = [str(cc), '-x', 'c', '-std=c11', '-O2', '-Wall', '-Wextra',
                   '-Werror', '-DEXPECTED_ABI_BYTES=' + ('8' if abi == 'lp64' else '4')]
            cmd += overlay_args
            cmd += ['-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += [str(cur_src), '-o', str(work / 'mutant.elf')]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            (work / f'{label}-{abi}.log').write_text(
                result.stdout + result.stderr, encoding='utf-8')
            ok = result.returncode != 0 and diagnostic in result.stderr
            print(f'sail-record-layout: {"PASS" if ok else "FAIL"} mutant {label}-{abi}')
            fails += not ok
    return fails


if __name__ == '__main__':
    raise SystemExit(main())
