#!/usr/bin/env python3
"""Build only the original-source SPU encoder parity driver; parent owns builds.

The separate Rust target validates original sources before invoking this
binary. Real-source generation/validation is run_combined_spu_source.py.
"""
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
    exe = out / ('combined-spu-encoder-fixture.exe' if sys.platform == 'win32' else 'combined-spu-encoder-fixture')
    flags = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(src)]
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    sources = [HERE / 'combined_spu_encoder_fixture.c', src / 'roll_combined.c', src / 'roll_ppu.c']
    subprocess.run(flags + list(map(str, sources)) + ['-o', str(exe)], check=True, timeout=180)
    print(f'PS3DK_PROF_ROLLING_COMBINED_SPU_ENCODER={exe}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
