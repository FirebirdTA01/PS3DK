#!/usr/bin/env python3
"""Compile and link the public POSIX APIs against an installed private SDK."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
if not os.environ.get('PS3DEV') or not os.environ.get('PS3DK'):
    print('posix time cross: SKIP (PS3DEV/PS3DK not set)')
    raise SystemExit(0)
dev, sdk = Path(os.environ['PS3DEV']), Path(os.environ['PS3DK'])
with tempfile.TemporaryDirectory(prefix='ps3dk-time-cross-') as temp:
    for abi, flags in [('ilp32', []), ('lp64', ['-mlp64'])]:
        for language, driver, std in [('c','gcc','gnu11'), ('c++','g++','gnu++17')]:
            for mode, defs in [('default', []), ('posix', ['-D_POSIX_C_SOURCE=200112L']), ('gnu', ['-D_GNU_SOURCE'])]:
                out = Path(temp)/f'{abi}-{language}-{mode}.elf'
                cmd=[str(dev/f'ppu/bin/powerpc64-ps3-elf-{driver}'), *flags, *defs,
                     '-x', language, '-std='+std, '-Werror', '-I'+str(sdk/'ppu/include'),
                     str(ROOT/'tests/sdk/posix-time-declarations.c'), '-o', str(out)]
                subprocess.run(cmd, check=True)
                print(f'posix time declarations/link {abi} {language} {mode}: PASS', flush=True)
