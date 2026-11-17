#!/usr/bin/env python3
"""Build test-only actual pfo/pfp/pfr integration fixture. Parent owns builds.

No product/runtime changes and no Rust checkpoint inputs. Run the explicit
rolling_ppu_c_output test target separately with the emitted executable path.
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
    exe = out / ('rolling-ppu-output-fixture.exe' if sys.platform == 'win32' else 'rolling-ppu-output-fixture')
    command = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        command += [f'-fsanitize={args.sanitize}', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    command += ['-I', str(impl), str(HERE / 'ppu_output_fixture.c')]
    command += [str(impl / name) for name in ['roll_ppu.c', 'roll_output.c', 'roll_store.c']]
    command += ['-o', str(exe)]
    subprocess.run(command, check=True, timeout=180)
    print(f'PS3DK_PROF_ROLLING_PPU_OUTPUT={exe}', flush=True)


if __name__ == '__main__':
    main()
