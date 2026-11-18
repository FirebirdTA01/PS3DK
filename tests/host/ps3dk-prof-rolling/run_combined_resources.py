#!/usr/bin/env python3
"""Build private combined capacity and actual pfo/pfr fixtures; caller owns builds."""
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
    out.mkdir(parents=True, exist_ok=False)
    src = ROOT / 'sdk/libps3dkprof/src/rolling'
    flags = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(src)]
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    for name, env, helpers in [
        ('combined_resource_fixture', 'PS3DK_PROF_ROLLING_COMBINED_RESOURCE', []),
        ('combined_output_fixture', 'PS3DK_PROF_ROLLING_COMBINED_OUTPUT', ['roll_output.c', 'roll_store.c']),
    ]:
        exe = out / (name + ('.exe' if sys.platform == 'win32' else ''))
        sources = [HERE / (name + '.c'), src / 'roll_combined.c', src / 'roll_ppu.c'] + [src / h for h in helpers]
        subprocess.run(flags + list(map(str, sources)) + ['-o', str(exe)], check=True, timeout=180)
        print(f'{env}={exe}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
