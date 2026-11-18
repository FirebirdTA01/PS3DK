#!/usr/bin/env python3
"""Build the private AUTO C transaction fixture with explicit dependencies.

Parent-owned gate only; no cargo or runtime producer invocation. The immutable
original-source Rust controls run separately through run_combined_auto.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(flags, sources):
    result = {str(Path(__file__).resolve()): digest(Path(__file__))}
    for source in sources:
        r = subprocess.run(flags + ['-M', '-MT', 'input', str(source)], capture_output=True,
                           text=True, timeout=60, check=True)
        paths = shlex.split(r.stdout.replace('\\\n', ' ').split(':', 1)[1])
        for name in paths:
            path = Path(name).resolve()
            result[str(path)] = digest(path)
    if len(result) > 4096:
        raise RuntimeError('compiler dependency count exceeds bound')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    out = a.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        raise RuntimeError('build directory must be external')
    out.mkdir(parents=True, exist_ok=False)
    src = ROOT / 'sdk/libps3dkprof/src/rolling'
    flags = shlex.split(a.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I', str(src)]
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    sources = [HERE / 'combined_auto_encoder_refusal.c', src / 'roll_combined2.c', src / 'roll_combined.c', src / 'roll_ppu.c']
    exe = out / ('combined-auto-encoder-fixture.exe' if sys.platform == 'win32' else 'combined-auto-encoder-fixture')
    command = flags + [str(p) for p in sources] + ['-o', str(exe)]
    before = inventory(flags, sources)
    receipt = dict(command=command, input_hashes=before)
    path = out / 'build.json'
    path.write_text(json.dumps(receipt, indent=2) + '\n')
    with (out / 'build.stdout').open('wb') as stdout, (out / 'build.stderr').open('wb') as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=180)
    receipt['compile_rc'] = result.returncode
    receipt['inputs_unchanged'] = before == inventory(flags, sources)
    if not result.returncode:
        receipt['executable_sha256'] = digest(exe)
    path.write_text(json.dumps(receipt, indent=2) + '\n')
    if result.returncode or not receipt['inputs_unchanged']:
        raise RuntimeError(f'compile failed or inputs changed: inspect {out}')
    print(f'PS3DK_PROF_ROLLING_COMBINED_AUTO_ENCODER={exe}', flush=True)


if __name__ == '__main__':
    main()
