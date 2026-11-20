#!/usr/bin/env python3
"""Build/run the independent terminal-root acceptance harness; caller owns build authority."""
import argparse
import pathlib
import shlex
import subprocess
import sys

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='cc')
    p.add_argument('--build-dir', type=pathlib.Path, required=True)
    args = p.parse_args()
    here = pathlib.Path(__file__).resolve().parent
    impl = here.parents[2] / 'sdk/libps3dkprof/src/rolling'
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    artifacts = out / 'artifacts'
    artifacts.mkdir(exist_ok=True)
    exe = out / ('rolling-terminal.exe' if sys.platform == 'win32' else 'rolling-terminal')
    subprocess.run(shlex.split(args.cc) + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic',
        '-I', str(impl), str(here / 'terminal_test.c'), str(impl / 'roll_store.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe), str(artifacts)])
    if result.returncode == 0:
        subprocess.run([sys.executable, str(here / 'decode.py'), '--self-test', '--terminal-dir', str(artifacts)], check=True)
    return result.returncode

if __name__ == '__main__':
    raise SystemExit(main())
