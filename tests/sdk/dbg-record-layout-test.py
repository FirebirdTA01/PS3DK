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
        print('dbg-record-layout: SKIP (set PS3DEV or --ps3dev)')
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
        with tempfile.TemporaryDirectory(prefix='dbg-layout-') as temp:
            return matrix(cc, args.include, Path(temp), root)
    work.mkdir(parents=True, exist_ok=False)
    return matrix(cc, args.include, work, root)


def matrix(cc, inc, work, root):
    fails = 0
    src = root / 'tests/sdk/dbg-record-layout-test.c'
    for lang, std in [('c', 'gnu99'), ('c', 'c11'), ('c++', 'c++17')]:
        for abi in ['ilp32', 'lp64']:
            label = f'{std}-{abi}'
            cmd = [str(cc), '-x', lang, '-std=' + std, '-O2', '-Wall',
                   '-Wextra', '-Werror', '-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += ['-c', str(src), '-o', str(work / (label + '.o'))]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            (work / (label + '.log')).write_text(
                repr(cmd) + '\n' + result.stdout + result.stderr,
                encoding='utf-8')
            print(f'dbg-record-layout: {"FAIL" if result.returncode else "PASS"} {label}')
            fails += bool(result.returncode)
    print(f'dbg-record-layout: {6 - fails}/6 PASS')
    if not fails:
        fails += mutants(cc, inc, work, src)
    return int(fails != 0)


def mutants(cc, inc, work, src):
    """Each old layout defect must fail its own ABI diagnostic."""
    nl = chr(10)
    cond_tail = ('    uint32_t wait_threads_num;' + nl + '    uint32_t wait_all_threads_num;' + nl
                 + '} sys_dbg_cond_information_t;')
    cases = [
        ('fpscr-before-fpr', 'sys/dbg.h',
         '    uint64_t fpr[SYS_DBG_PPU_FPR_NUM];' + nl + '    uint32_t fpscr;' + nl,
         '    uint32_t fpscr;' + nl + '    uint64_t fpr[SYS_DBG_PPU_FPR_NUM];' + nl,
         'sys_dbg_ppu_thread_context_t.fpr offset', ['ilp32', 'lp64']),
        ('spu-cfg-32-bit', 'sys/dbg.h',
         '    uint32_t fpscr;' + nl + '    uint32_t srr0;' + nl + '    uint32_t spu_status;' + nl + '    uint64_t spu_cfg;' + nl,
         '    uint32_t fpscr;' + nl + '    uint32_t srr0;' + nl + '    uint32_t spu_status;' + nl + '    uint32_t spu_cfg;' + nl,
         'sys_dbg_spu_thread_context_t.mb_stat offset', ['ilp32', 'lp64']),
        ('wide-wait-list', 'sys/dbg.h',
         '    sys_ppu_thread_t *wait_id_list ATTRIBUTE_PRXPTR;' + nl + cond_tail,
         '    sys_ppu_thread_t *wait_id_list;' + nl + cond_tail,
         'sys_dbg_cond_information_t.wait_id_list width', ['lp64']),
    ]
    fails = 0
    for label, header, old, new, diagnostic, abis in cases:
        text = (inc / header).read_text(encoding='utf-8')
        if text.count(old) != 1:
            print(f'dbg-record-layout: FAIL cannot plant {label}')
            fails += 1
            continue
        overlay = work / label
        out = overlay / header
        out.parent.mkdir(parents=True)
        out.write_text(text.replace(old, new), encoding='utf-8')
        for abi in abis:
            cmd = [str(cc), '-x', 'c', '-std=c11', '-Wall', '-Wextra',
                   '-Werror', '-I' + str(overlay), '-I' + str(inc)]
            if abi == 'lp64':
                cmd.append('-mlp64')
            cmd += ['-c', str(src), '-o', str(work / 'mutant.o')]
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    errors='replace', timeout=60)
            (work / f'{label}-{abi}.log').write_text(
                result.stdout + result.stderr, encoding='utf-8')
            ok = result.returncode != 0 and diagnostic in result.stderr
            print(f'dbg-record-layout: {"PASS" if ok else "FAIL"} mutant {label}-{abi}')
            fails += not ok
    return fails


if __name__ == '__main__':
    raise SystemExit(main())
