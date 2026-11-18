#!/usr/bin/env python3
"""Build private combined RED surface plus unchanged standalone PPU fixture.

Runs all named C cases even when some fail. Rust artifact comparisons are a
separate parent invocation; this script never invokes cargo.
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
    p.add_argument('--sanitize', choices=('address', 'undefined'))
    a = p.parse_args()
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    src = ROOT / 'sdk/libps3dkprof/src/rolling'
    flags = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(src)]
    if a.sanitize:
        flags += [f'-fsanitize={a.sanitize}', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    binaries = {}
    for name, inputs in [
        ('combined-encoder-test', [HERE / 'combined_encoder_test.c', src / 'roll_combined.c', src / 'roll_ppu.c']),
        ('combined-encoder-fixture', [HERE / 'combined_encoder_fixture.c', src / 'roll_combined.c', src / 'roll_ppu.c']),
        ('ppu-seam-fixture', [HERE / 'ppu_encoder_fixture.c', src / 'roll_ppu.c']),
    ]:
        exe = out / (name + ('.exe' if sys.platform == 'win32' else ''))
        subprocess.run(flags + list(map(str, inputs)) + ['-o', str(exe)], check=True, timeout=180)
        binaries[name] = exe
    print(f'PS3DK_PROF_ROLLING_COMBINED_ENCODER={binaries["combined-encoder-fixture"]}', flush=True)
    print(f'PS3DK_PROF_ROLLING_PPU_SEAM={binaries["ppu-seam-fixture"]}', flush=True)
    failures = 0
    for case in ('ppu-seam', 'planner', 'undersized', 'overflow', 'initial', 'flags', 'init-short-cap'):
        rc = subprocess.run([str(binaries['combined-encoder-test']), case], timeout=60).returncode
        print(f'{case}: rc={rc}', flush=True)
        failures += rc != 0
    print(f'C combined surface: {7 - failures} passed, {failures} failed', flush=True)
    return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
