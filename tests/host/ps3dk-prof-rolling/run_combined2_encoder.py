#!/usr/bin/env python3
"""Parent-owned build/run gate for private FIFO schema2 refusal/encoder surface.

No cargo invocation. Requires a fresh external build directory; keeps all logs
and dependency/command/executable receipts, including failed builds and tests.
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


def dependencies(flags, inputs):
    paths = {Path(__file__).resolve()}
    for source in inputs:
        r = subprocess.run(flags + ['-M', '-MT', 'input', str(source)],
                           check=True, capture_output=True, text=True, timeout=60)
        names = shlex.split(r.stdout.replace('\\\n', ' ').split(':', 1)[1])
        paths.update(Path(p).resolve() for p in names)
    if len(paths) > 4096:
        raise RuntimeError('dependency inventory exceeds bound')
    return {str(p): digest(p) for p in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', choices=('address', 'undefined'))
    args = parser.parse_args()
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        raise RuntimeError('build directory must be external')
    out.mkdir(parents=True, exist_ok=False)
    src = ROOT / 'sdk/libps3dkprof/src/rolling'
    flags = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra',
        '-Werror', '-pedantic', '-I', str(src)]
    if args.sanitize:
        flags += [f'-fsanitize={args.sanitize}', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
    binaries = {}
    for name in ('test', 'fixture', 'projection'):
        inputs = ([HERE / 'combined2_projection_test.c'] if name == 'projection' else
                  [HERE / f'combined2_encoder_{name}.c', src / 'roll_combined2.c'])
        inputs += [src / 'roll_combined.c', src / 'roll_ppu.c']
        exe = out / ('combined2-encoder-' + name + ('.exe' if sys.platform == 'win32' else ''))
        command = flags + list(map(str, inputs)) + ['-o', str(exe)]
        before = dependencies(flags, inputs)
        receipt = dict(command=command, input_hashes=before)
        path = out / (name + '-build.json')
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        with (out / (name + '-build.stdout')).open('wb') as stdout, (out / (name + '-build.stderr')).open('wb') as stderr:
            r = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=180)
        receipt['compile_rc'] = r.returncode
        receipt['inputs_unchanged'] = dependencies(flags, inputs) == before
        if not r.returncode:
            receipt['executable_sha256'] = digest(exe)
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        if r.returncode or not receipt['inputs_unchanged']:
            raise RuntimeError(f'{name}: compilation failed or compiler inputs changed; inspect {out}')
        binaries[name] = exe
    print(f'PS3DK_PROF_ROLLING_COMBINED2_ENCODER={binaries["fixture"]}', flush=True)
    results = []
    for case in ('planner', 'overflow', 'undersized', 'projection'):
        with (out / (case + '.stdout')).open('wb') as stdout, (out / (case + '.stderr')).open('wb') as stderr:
            command = [str(binaries['projection'])] if case == 'projection' else [str(binaries['test']), case]
            r = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=60)
        results.append(dict(case=case, rc=r.returncode))
        print(f'{case}: rc={r.returncode}', flush=True)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(any(r['rc'] for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
