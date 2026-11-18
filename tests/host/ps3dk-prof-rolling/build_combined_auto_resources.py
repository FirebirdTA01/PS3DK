#!/usr/bin/env python3
"""Parent-owned bounded build of two independent AUTO resource probes."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

from build_combined_auto import ROOT, HERE, digest, inventory


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
    suffix = '.exe' if sys.platform == 'win32' else ''
    for name, env in [('resource', 'RESOURCE'), ('work', 'WORK')]:
        sources = [HERE / f'combined_auto_{name}_fixture.c', src / 'roll_combined2.c', src / 'roll_combined.c', src / 'roll_ppu.c']
        exe = out / f'combined-auto-{name}-fixture{suffix}'
        command = flags + list(map(str, sources)) + ['-o', str(exe)]
        before = inventory(flags, sources)
        before[str(Path(__file__).resolve())] = digest(Path(__file__))
        receipt = dict(command=command, input_hashes=before)
        path = out / f'{name}.json'
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        with (out / f'{name}.stdout').open('wb') as stdout, (out / f'{name}.stderr').open('wb') as stderr:
            result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=180)
        after = inventory(flags, sources)
        after[str(Path(__file__).resolve())] = digest(Path(__file__))
        receipt['compile_rc'] = result.returncode
        receipt['inputs_unchanged'] = before == after
        if not result.returncode:
            receipt['executable_sha256'] = digest(exe)
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        if result.returncode or not receipt['inputs_unchanged']:
            raise RuntimeError(f'compile failed or inputs changed: inspect {path}')
        print(f'PS3DK_PROF_ROLLING_COMBINED_AUTO_{env}={exe}', flush=True)


if __name__ == '__main__':
    main()
