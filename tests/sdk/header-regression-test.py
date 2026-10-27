#!/usr/bin/env python3
"""Compile five header regression guards with a real PPU compiler (no execution)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / 'tests/sdk'
INCLUDE = ROOT / 'sdk/include'


def driver(name, override):
    if override:
        found = shutil.which(override)
        if not found:
            raise RuntimeError(f'explicit compiler unavailable: {override}')
        return found
    for variable in ('PS3DEV', 'PS3DK'):
        if os.environ.get(variable):
            base = Path(os.environ[variable]) / 'ppu/bin' / name
            for candidate in (base, base.with_suffix('.exe')):
                if candidate.is_file():
                    return str(candidate)
    return shutil.which(name)


def cases():
    for order in ('NETINET_THEN_LIBNETCTL', 'LIBNETCTL_THEN_NETINET',
                  'NETEX_THEN_NETINET', 'NETINET_THEN_NETEX', 'ALL'):
        yield 'in-addr-redefinition', order, 'TEST_ORDER_' + order, ('gnu99', 'c11', 'c++17')
    for order in ('SYS_TYPES_THEN_PTHREAD', 'PTHREAD_THEN_SYS_TYPES', 'DEFAULT'):
        yield 'pthread-iso-types', order, 'TEST_ORDER_' + order, ('c99', 'c11', 'gnu11', 'c++17')
    for header in ('CELL_SPURS', 'SYS_DBG'):
        yield 'sys-process-param-visibility', header, 'TEST_INCLUDE_' + header, ('gnu99', 'c11', 'c++17')
    for name in ('sys-vm', 'spu-thread-events'):
        yield name, 'surface', None, ('gnu99', 'c11', 'c++17')


def run_matrix(cc, cxx, output):
    for compiler in (cc, cxx):
        target = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
        if target != 'powerpc64-ps3-elf':
            raise RuntimeError(f'expected PPU compiler, got {target}: {compiler}')
        print(subprocess.check_output([compiler, '--version'], text=True).splitlines()[0])

    def compile_case(name, define, mode, abi, folder, overlay=None):
        folder.mkdir(parents=True)
        cmd = [cxx if '++' in mode else cc, '-x', 'c++' if '++' in mode else 'c',
               '-std=' + mode, '-O2', '-Wall', '-Wextra', '-Werror']
        if abi == 'lp64':
            cmd.append('-mlp64')
        if overlay:
            cmd += ['-I', str(overlay)]
        cmd += ['-I', str(INCLUDE)]
        if define:
            cmd.append('-D' + define + '=1')
        cmd += ['-c', str(SOURCES / (name + '-test.c')), '-o', str(folder / 'probe.o')]
        (folder / 'command.txt').write_text(repr(cmd) + '\n', encoding='utf-8')
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (folder / 'compiler.log').write_text(result.stdout, encoding='utf-8')
        return result

    checks = 0
    for name, label, define, modes in cases():
        for abi in ('ilp32', 'lp64'):
            for mode in modes:
                row = f'{name}-{label}-{abi}-{mode}'
                result = compile_case(name, define, mode, abi, output / row)
                if result.returncode:
                    raise RuntimeError(f'{row}: compile failed\n{result.stdout}')
                print(f'PASS {row}', flush=True)
                checks += 1

    # Each overlay includes the real header, then introduces exactly one defect.
    # These are synthetic sensitivity controls, not historical/runtime failures.
    defects = (
        ('in-addr-redefinition', 'TEST_ORDER_NETINET_THEN_LIBNETCTL', 'cell/libnetctl.h',
         'struct in_addr { unsigned int s_addr; };', 'in_addr'),
        ('pthread-iso-types', 'TEST_ORDER_SYS_TYPES_THEN_PTHREAD', 'pthread.h',
         '#define pthread_t missing_pthread_type', 'missing_pthread_type'),
        ('sys-process-param-visibility', 'TEST_INCLUDE_SYS_DBG', 'sys/dbg.h',
         '#undef SYS_PROCESS_PARAM', 'SYS_PROCESS_PARAM'),
        ('sys-vm', None, 'sys/vm.h',
         '#undef SYS_VM_STATE_UNUSED\n#define SYS_VM_STATE_UNUSED 8', 'SYS_VM_STATE_UNUSED mismatch'),
        ('spu-thread-events', None, 'sys/spu_thread.h',
         '#undef SYS_SPU_THREAD_EVENT_DMA\n#define SYS_SPU_THREAD_EVENT_DMA 8',
         'SYS_SPU_THREAD_EVENT_DMA value mismatch'),
    )
    negatives = 0
    for name, define, header, defect, diagnostic in defects:
        overlay = output / ('mutant-' + name)
        target = overlay / header
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f'#include "{(INCLUDE / header).as_posix()}"\n{defect}\n', encoding='utf-8')
        for abi in ('ilp32', 'lp64'):
            result = compile_case(name, define, 'c11', abi, output / ('reject-' + name + '-' + abi), overlay)
            if result.returncode == 0 or diagnostic not in result.stdout:
                raise RuntimeError(f'{name}/{abi}: defect not rejected as expected\n{result.stdout}')
            print(f'PASS rejects {name}/{abi}: {diagnostic}', flush=True)
            negatives += 1
    print(f'header-regression: PASS {checks} compiles, {negatives} defect controls')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-toolchain', action='store_true', help='fail instead of skipping missing drivers')
    parser.add_argument('--output', type=Path, help='retain commands, diagnostics and objects in a new directory')
    args = parser.parse_args()
    cc = driver('powerpc64-ps3-elf-gcc', os.environ.get('PPU_CC'))
    cxx = driver('powerpc64-ps3-elf-g++', os.environ.get('PPU_CXX'))
    if not cc or not cxx:
        if args.require_toolchain:
            raise RuntimeError('PPU C and C++ compilers are required')
        print('header-regression: SKIP (PPU C/C++ compilers unavailable; no target checks ran)')
        return
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        run_matrix(cc, cxx, output)
    else:
        with tempfile.TemporaryDirectory(prefix='ps3-header-guards-') as directory:
            run_matrix(cc, cxx, Path(directory))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f'header-regression: FAIL: {error}')
