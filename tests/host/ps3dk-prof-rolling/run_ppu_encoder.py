#!/usr/bin/env python3
"""Build private C PPU encoder tests/fixture; parent owns build authority.

Always builds both binaries before running the standalone test, including RED.
The Rust original-source parity test is a separate explicit invocation.
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
    args = p.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    impl = ROOT / 'sdk/libps3dkprof/src/rolling'
    common = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        common += [f'-fsanitize={args.sanitize}', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    common += ['-I', str(impl)]
    binaries = []
    for source, name in [('ppu_encoder_test.c', 'rolling-ppu-encoder-test'),
                         ('ppu_encoder_fixture.c', 'rolling-ppu-encoder-fixture')]:
        exe = out / (name + ('.exe' if sys.platform == 'win32' else ''))
        subprocess.run(common + [str(HERE / source), str(impl / 'roll_ppu.c'), '-o', str(exe)], check=True, timeout=180)
        binaries.append(exe)
    print(f'PS3DK_PROF_ROLLING_PPU_ENCODER={binaries[1]}', flush=True)
    return subprocess.run([str(binaries[0])], timeout=180).returncode


if __name__ == '__main__':
    raise SystemExit(main())
