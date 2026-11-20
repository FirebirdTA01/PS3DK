#!/usr/bin/env python3
"""Build exact retained-metadata-capacity fixture; caller owns compiler/lease."""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    out = a.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        raise RuntimeError('external build directory required')
    out.mkdir(parents=True, exist_ok=False)
    src = ROOT / 'sdk/libps3dkprof/src/rolling'
    flags = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(src)]
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    exe = out / ('metadata-fixture.exe' if sys.platform == 'win32' else 'metadata-fixture')
    sources = [HERE / 'combined2_metadata_fixture.c'] + [src / n for n in ['roll_combined2.c', 'roll_combined.c', 'roll_ppu.c']]
    result = subprocess.run(flags + list(map(str, sources)) + ['-o', str(exe)], capture_output=True, timeout=180)
    (out / 'compile.log').write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    print(f'PS3DK_PROF_ROLLING_COMBINED2_METADATA={exe}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
