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
        print('codec-record-layout: SKIP (set PS3DEV or --ps3dev)')
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
        with tempfile.TemporaryDirectory(prefix='codec-layout-') as temp:
            return matrix(cc, args.include, Path(temp), root)
    work.mkdir(parents=True, exist_ok=False)
    return matrix(cc, args.include, work, root)


def matrix(cc, inc, work, root):
    fails = 0
    src = root / 'tests/sdk/codec-record-layout-test.c'
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
            print(f'codec-record-layout: {"FAIL" if result.returncode else "PASS"} {label}')
            fails += bool(result.returncode)
    print(f'codec-record-layout: {6 - fails}/6 PASS')
    if not fails:
        fails += mutants(cc, inc, work, src)
    return int(fails != 0)


def mutants(cc, inc, work, src):
    """Each old layout defect and witness corruption must fail its own diagnostic."""
    nl = chr(10)
    cases = [
        ('gifdec-thread-in-param-size', 'test:',
         '/* Mutant controls for test runner verification */',
         '#define MUTANT_GIFDEC_THREAD_IN_PARAM_SIZE 1' + nl + '/* Mutant controls for test runner verification */',
         'mutant: old 40-byte thread in param', ['ilp32', 'lp64']),
        ('gifdec-src-size', 'test:',
         '/* Mutant controls for test runner verification */',
         '#define MUTANT_GIFDEC_SRC_SIZE 1' + nl + '/* Mutant controls for test runner verification */',
         'mutant: old 40-byte src size', ['ilp32', 'lp64']),
        ('jpgenc-output-param-size', 'test:',
         '/* Mutant controls for test runner verification */',
         '#define MUTANT_JPGENC_OUTPUT_PARAM_SIZE 1' + nl + '/* Mutant controls for test runner verification */',
         'mutant: old 24-byte jpgenc output param', ['ilp32', 'lp64']),
        ('jpgenc-stream-info-streamaddr', 'test:',
         '/* Mutant controls for test runner verification */',
         '#define MUTANT_JPGENC_STREAM_INFO_STREAMADDR 1' + nl + '/* Mutant controls for test runner verification */',
         'mutant: old jpgenc streamAddr offset', ['ilp32', 'lp64']),
        ('pngenc-output-param-size', 'test:',
         '/* Mutant controls for test runner verification */',
         '#define MUTANT_PNGENC_OUTPUT_PARAM_SIZE 1' + nl + '/* Mutant controls for test runner verification */',
         'mutant: old 24-byte pngenc output param', ['ilp32', 'lp64']),
        ('pngenc-stream-info-streamaddr', 'test:',
         '/* Mutant controls for test runner verification */',
         '#define MUTANT_PNGENC_STREAM_INFO_STREAMADDR 1' + nl + '/* Mutant controls for test runner verification */',
         'mutant: old pngenc streamAddr offset', ['ilp32', 'lp64']),
        ('gifdec-thread-in-param-malloc-pointer', 'cell/codec/gifdec.h',
         'void *cbCtrlMallocArg ATTRIBUTE_PRXPTR;',
         'void *cbCtrlMallocArg;',
         'CellGifDecThreadInParam size', ['lp64']),
        ('gifdec-src-filename-pointer', 'cell/codec/gifdec.h',
         'const char *fileName ATTRIBUTE_PRXPTR;',
         'const char *fileName;',
         'CellGifDecSrc size', ['lp64']),
        ('jpgenc-output-param-filename-pointer', 'cell/codec/jpgenc.h',
         'typedef struct { CellJpgEncLocation location; const char *streamFileName ATTRIBUTE_PRXPTR; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize; } CellJpgEncOutputParam;',
         'typedef struct { CellJpgEncLocation location; const char *streamFileName; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize; } CellJpgEncOutputParam;',
         'CellJpgEncOutputParam size', ['lp64']),
        ('pngenc-output-param-filename-pointer', 'cell/codec/pngenc.h',
         'typedef struct { CellPngEncLocation location; const char *streamFileName ATTRIBUTE_PRXPTR; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize; } CellPngEncOutputParam;',
         'typedef struct { CellPngEncLocation location; const char *streamFileName; void *streamAddr ATTRIBUTE_PRXPTR; uint32_t limitSize; } CellPngEncOutputParam;',
         'CellPngEncOutputParam size', ['lp64']),
        ('jpgdec-strm-param-pointer', 'cell/codec/jpgdec.h',
         '{ void *strmPtr ATTRIBUTE_PRXPTR; uint32_t strmSize; } CellJpgDecStrmParam;',
         '{ void *strmPtr; uint32_t strmSize; } CellJpgDecStrmParam;',
         'CellJpgDecStrmParam.strmPtr width', ['lp64']),
        ('pngdec-strm-param-pointer', 'cell/codec/pngdec.h',
         '    void    *strmPtr ATTRIBUTE_PRXPTR;' + nl + '    uint32_t strmSize;',
         '    void    *strmPtr;' + nl + '    uint32_t strmSize;',
         'CellPngDecStrmParam.strmPtr width', ['lp64']),
        ('jpgdec-disp-info-image-pointer', 'cell/codec/jpgdec.h',
         '    void    *outputImage ATTRIBUTE_PRXPTR;' + nl + '} CellJpgDecDispInfo;',
         '    void    *outputImage;' + nl + '} CellJpgDecDispInfo;',
         'CellJpgDecDispInfo.outputImage width', ['lp64']),
        ('pngdec-disp-info-image-pointer', 'cell/codec/pngdec.h',
         '    void     *outputImage ATTRIBUTE_PRXPTR;' + nl + '} CellPngDecDispInfo;',
         '    void     *outputImage;' + nl + '} CellPngDecDispInfo;',
         'CellPngDecDispInfo.outputImage width', ['lp64']),
        ('witness-always-fail', 'test:',
         '    return 0;' + nl + '}',
         '    codec_witness_decode_mismatch();' + nl + '    return 0;' + nl + '}',
         'codec_witness_decode_mismatch', ['ilp32', 'lp64']),
        ('witness-corrupted-filename', 'test:',
         'gsrc_buf[4] = 0x20;',
         'gsrc_buf[4] = 0x99;',
         'codec_witness_decode_mismatch', ['ilp32', 'lp64']),
        ('witness-corrupted-callback', 'test:',
         'gtip_buf[16] = 0x10;',
         'gtip_buf[16] = 0x99;',
         'codec_witness_decode_mismatch', ['ilp32', 'lp64']),
    ]
    fails = 0
    for label, target_file, old, new, diagnostic, abis in cases:
        if target_file == 'test:':
            text = src.read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'codec-record-layout: FAIL cannot plant {label}')
                fails += 1
                continue
            mutant_src = work / f'{label}.c'
            mutant_src.write_text(text.replace(old, new), encoding='utf-8')
            cur_src = mutant_src
            overlay_args = []
        else:
            text = (inc / target_file).read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'codec-record-layout: FAIL cannot plant {label}')
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
            print(f'codec-record-layout: {"PASS" if ok else "FAIL"} mutant {label}-{abi}')
            fails += not ok
    return fails


if __name__ == '__main__':
    raise SystemExit(main())
