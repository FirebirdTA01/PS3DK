#!/usr/bin/env python3
"""GPU API context-type matrix (P3.3 ctx-type fix; review 00:14 GO + carries).

--target: the PPU compilers.  POSITIVE (each must compile with NO diagnostic, -Werror): C11 in both include orders;
C++ gnu++17 and c++20 in both include orders.  NEGATIVE (each must FAIL, and its FIRST error must be the pointer
diagnostic of the call, not an include or setup failure): an int * into each of the six context-taking
declarations, in C and in C++.  OLD-DEFECT control: the sealed pre-fix header (ctxtype/old, blob 13b41649) compiled in
C with gCellGcmCurrentContext must still give -Wincompatible-pointer-types.  --archive/--libs: the C++ caller
(R4B_MAIN) also LINKS against the C runtime archive (the extern "C" boundary; never run).
--host: prof_gpu.h compiles in host C and C++ with no cell headers (-Werror).

usage: ctx-test.py --target CC CXX SRC_DIR INCLUDE_DIR SYSTEM_INCLUDE_DIR CONFIG_H OUTDIR [--archive A --libs "..."]
       ctx-test.py --host CC CXX SRC_DIR INCLUDE_DIR OUTDIR          (exit 0 iff every row holds)"""
import os
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMMON = ['-O2', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=1']
C_PTR = 'incompatible-pointer-types'
CXX_PTR = ('invalid conversion', 'cannot convert')


def run(cmd, log):
    r = subprocess.run(cmd, capture_output=True, text=True)
    Path(log).write_text(' '.join(map(str, cmd)) + '\n--- rc %d\n%s%s' % (r.returncode, r.stdout, r.stderr))
    return r


def first_error(stderr):
    for l in stderr.splitlines():
        if 'error:' in l:
            return l
    return ''


def main():
    a = sys.argv[1:]
    archive = libs = None
    if '--archive' in a:
        i = a.index('--archive')
        archive, libs = a[i + 1], a[a.index('--libs') + 1]
        a = a[:i]
    mode = a[0] if a else ''
    rows = []
    if mode == '--target' and len(a) == 8:
        cc, cxx, src, inc, sysinc, config, out = a[1:]
        out = Path(out)
        out.mkdir()
        incs = [f'-I{inc}', f'-I{sysinc}', f'-I{src}/rsx', f'-I{src}/core', f'-I{src}/spu', f'-I{src}/pal']
        pre = ['-mcpu=cell', '-include', config]

        def pos(name, comp, std, srcf, extra):
            r = run([comp, *COMMON, *pre, std, *extra, *incs, '-c', str(HERE / srcf), '-o', str(out / f'{name}.o')],
                    out / f'{name}.log')
            ok = r.returncode == 0 and not r.stderr.strip()
            rows.append((name, ok, 'clean' if ok else f'rc {r.returncode}: {first_error(r.stderr) or r.stderr[:160]}'))

        def neg(name, comp, std, srcf, extra, want):
            r = run([comp, *COMMON, *pre, std, *extra, *incs, '-c', str(HERE / srcf), '-o', os.devnull],
                    out / f'{name}.log')
            e = first_error(r.stderr)
            ok = r.returncode != 0 and any(w in e for w in (want if isinstance(want, tuple) else (want,)))
            rows.append((name, ok, e[:200] or f'rc {r.returncode}, no error'))

        for order in (1, 2):
            pos(f'c11-order{order}', cc, '-std=c11', 'ctx_api.c', [f'-DR4B_ORDER={order}'])
            for std in ('-std=gnu++17', '-std=c++20'):
                pos(f'cxx{std[5:]}-order{order}', cxx, std, 'ctx_api.cc', [f'-DR4B_ORDER={order}'])
        for k in range(1, 7):
            neg(f'neg-c-decl{k}', cc, '-std=c11', 'ctx_api.c', ['-DR4B_ORDER=1', f'-DR4B_NEG={k}'], C_PTR)
            neg(f'neg-cxx-decl{k}', cxx, '-std=gnu++17', 'ctx_api.cc', ['-DR4B_ORDER=1', f'-DR4B_NEG={k}'], CXX_PTR)
        # the old header shadows the new one: its directory comes first
        r = run([cc, *COMMON, *pre, '-std=c11', '-DR4B_ORDER=1', '-DR4B_OLD', f'-I{HERE / "old"}', *incs, '-c',
                 str(HERE / 'ctx_api.c'), '-o', os.devnull], out / 'old-defect.log')
        e = first_error(r.stderr)
        rows.append(('old-header defect still reproduces', r.returncode != 0 and C_PTR in e, e[:200]))
        if archive:
            o = out / 'cxx-main.o'
            r = run([cxx, *COMMON, *pre, '-std=gnu++17', '-DR4B_ORDER=1', '-DR4B_MAIN', *incs, '-c',
                     str(HERE / 'ctx_api.cc'), '-o', str(o)], out / 'cxx-main-cc.log')
            ok = r.returncode == 0 and not r.stderr.strip()
            if ok:
                r = run([cxx, '-mcpu=cell', '-Wl,--gc-sections', str(o), archive, *shlex.split(libs), '-o',
                         str(out / 'cxx-main.elf')], out / 'cxx-main-link.log')
                ok = r.returncode == 0 and (out / 'cxx-main.elf').stat().st_size > 0
            rows.append(('C++ caller links against the C runtime archive (extern "C")', ok,
                         'linked' if ok else (first_error(r.stderr) or r.stderr[:200])))
    elif mode == '--host' and len(a) == 6:
        cc, cxx, src, inc, out = a[1:]
        out = Path(out)
        out.mkdir()
        for name, comp, std, srcf in (('host-c11', cc, '-std=c11', 'ctx_host.c'),
                                      ('host-cxx17', cxx, '-std=gnu++17', 'ctx_host.cc')):
            r = run([comp, *COMMON, std, f'-I{inc}', f'-I{src}/rsx', '-c', str(HERE / srcf), '-o',
                     str(out / f'{name}.o')], out / f'{name}.log')
            ok = r.returncode == 0 and not r.stderr.strip()
            rows.append((name + ' (no cell headers)', ok, 'clean' if ok else first_error(r.stderr) or r.stderr[:160]))
    else:
        print(__doc__)
        return 2
    bad = 0
    for name, ok, why in rows:
        print(f'{"PASS" if ok else "FAIL"} {name}: {why}')
        bad += not ok
    print(f'CTX-TYPE {mode[2:].upper()} ' + ('PASS' if not bad else f'FAIL ({bad})') + f' ({len(rows)} rows)')
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
