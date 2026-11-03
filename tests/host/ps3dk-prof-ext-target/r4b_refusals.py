#!/usr/bin/env python3
"""P3.3 R4b configuration refusals (host preprocessor).  Each refused configuration of ext_runtime.c / gpu_runtime.c
must fail, and its FIRST diagnostic must be its own #error text (a refusal for the named reason, not an unrelated
failure); each host configuration the suites use must preprocess cleanly.  The refusals sit before any include, so the
host compiler reaches them; --target re-checks them with the PPU compiler.

usage: r4b_refusals.py [--target] CC SRC_DIR INCLUDE_DIR [SYSTEM_INCLUDE_DIR]      (exit 0 iff every row holds)
--target uses the PPU compiler rows (LV2 clean; LV2 + -mlp64 / hooks / devmem, HOST+LV2 and no PAL refused)."""
import os
import subprocess
import sys

H = ['-DPS3DK_PROF=1', '-DPS3DK_PROF_WITH_SPU']
LV2 = ['-DPS3DK_PROF_PAL_LV2', '-U__LP64__']  # the host is LP64; undefining models the ILP32 PPU for the later rows
ROWS = [  # (file stem, extra flags, None = must preprocess cleanly, or the exact #error text)
    ('host', ['-DPS3DK_PROF_PAL_HOST'], None),
    ('host+hooks', ['-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS'], None),
    ('host+hooks+devmem', ['-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DGPP_HOST_DEVMEM'], None),
    ('host+lv2', ['-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_PAL_LV2'], '{x}: PS3DK_PROF_PAL_HOST and PS3DK_PROF_PAL_LV2 together'),
    ('no-pal', [], '{x}: no PAL selected'),
    ('lv2+lp64', ['-DPS3DK_PROF_PAL_LV2'], '{x} target is ILP32 only'),
    ('lv2+hooks', LV2 + ['-DPS3DK_PROF_TEST_HOOKS'], '{x} target never carries test hooks'),
    ('lv2+devmem', LV2 + ['-DGPP_HOST_DEVMEM'], 'GPP_HOST_DEVMEM is a host-only test seam'),
]
# --target: the PPU compiler (ILP32 by default, -mlp64 for LP64); the LV2 configuration must preprocess cleanly
TARGET_ROWS = [
    ('lv2', ['-DPS3DK_PROF_PAL_LV2'], None),
    ('lv2+mlp64', ['-DPS3DK_PROF_PAL_LV2', '-mlp64'], '{x} target is ILP32 only'),
    ('lv2+hooks', ['-DPS3DK_PROF_PAL_LV2', '-DPS3DK_PROF_TEST_HOOKS'], '{x} target never carries test hooks'),
    ('lv2+devmem', ['-DPS3DK_PROF_PAL_LV2', '-DGPP_HOST_DEVMEM'], 'GPP_HOST_DEVMEM is a host-only test seam'),
    ('host+lv2', ['-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_PAL_LV2'], '{x}: PS3DK_PROF_PAL_HOST and PS3DK_PROF_PAL_LV2 together'),
    ('no-pal', [], '{x}: no PAL selected'),
]
FILES = {'spu/ext_runtime.c': 'the SPU lane extension', 'rsx/gpu_runtime.c': 'the GPU domain extension'}


def main():
    args = sys.argv[1:]
    target = bool(args) and args[0] == '--target'
    if target:
        args = args[1:]
    if len(args) not in (3, 4):
        print(__doc__)
        return 2
    cc, src, inc = args[:3]
    incs = [f'-I{inc}'] + [f'-I{src}/{d}' for d in ('core', 'pal', 'spu', 'rsx')]
    if len(args) == 4:
        incs.append(f'-I{args[3]}')
    rows = TARGET_ROWS if target else ROWS
    fails = 0
    for f, x in FILES.items():
        for name, extra, want in rows:
            r = subprocess.run([cc, '-E', '-std=c11', *H, *extra, *incs, f'{src}/{f}', '-o', os.devnull],
                               capture_output=True, text=True)
            errs = [l for l in r.stderr.splitlines() if 'error:' in l]
            if want is None:
                ok = r.returncode == 0 and not errs
                got = 'clean' if ok else f'rc {r.returncode}: {errs[:1]}'
            else:
                text = want.format(x=x)
                ok = r.returncode != 0 and bool(errs) and errs[0].endswith(f'#error "{text}"')
                got = errs[0].split('error:', 1)[1].strip() if errs else f'rc {r.returncode}, no error'
            print(f'{"PASS" if ok else "FAIL"} {f} {name}: {got}')
            fails += not ok
    print('R4B REFUSALS ' + ('PASS' if not fails else f'FAIL ({fails})'))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
