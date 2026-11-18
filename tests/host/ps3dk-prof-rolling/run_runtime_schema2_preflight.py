#!/usr/bin/env python3
"""Build a separate actual selector/preflight fixture; parent owns compilation.

Reuse the complete native/core recipe without editing its running source files.
The new fixture is copied byte-for-byte under the recipe's fixed input basename.
The outer provenance receipt binds those copies to their real source files;
the recipe records compiler flags, complete -M dependencies and executable hash.
"""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import run_runtime_schema2 as recipe
from run_runtime_spu import sha

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--sanitize', choices=('address', 'undefined'))
    a = p.parse_args()
    if os.name != 'posix':
        raise RuntimeError('Linux/WSL host fixture only')
    out = a.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    originals = [Path(__file__).resolve(), HERE / 'runtime_schema2_preflight_test.c',
                 HERE / 'run_runtime_schema2.py', HERE / 'run_runtime_spu.py',
                 HERE.parent / 'ps3dk-prof-runtime/auto-ring-runtime-test.c']
    pins = {str(p): sha(p) for p in originals}
    leaf = out / 'fixture-source/tests/host/ps3dk-prof-rolling'
    leaf.mkdir(parents=True)
    port = leaf.parent / 'ps3dk-prof-runtime'
    port.mkdir()
    copies = [(HERE / 'runtime_schema2_preflight_test.c', leaf / 'runtime_schema2_test.c'),
              (HERE / 'run_runtime_spu.py', leaf / 'run_runtime_spu.py'),
              (originals[-1], port / 'auto-ring-runtime-test.c')]
    for source, target in copies:
        shutil.copyfile(source, target)
        if sha(target) != pins[str(source)]:
            raise RuntimeError('fixture input changed while copying')
    receipt = dict(original_inputs=pins,
                   unmodified_fixture_copies={str(t): str(s) for s, t in copies},
                   scope='actual READY/preflight only; no terminal capture claim')
    (out / 'fixture-inputs.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if any(sha(Path(p)) != digest for p, digest in pins.items()):
        raise RuntimeError('original inputs changed before recipe')
    # ROOT and all product source paths remain the recipe's original worktree.
    # Only its fixture location changes, to the explicit unchanged copies above.
    recipe.HERE = leaf
    sys.argv = [str(HERE / 'run_runtime_schema2.py'), '--build-dir', str(out / 'build'), '--cc', a.cc]
    if a.sanitize:
        sys.argv += ['--sanitize', a.sanitize]
    transcript = io.StringIO()
    try:
        with contextlib.redirect_stdout(transcript):
            recipe.main()
    finally:
        (out / 'recipe.stdout').write_text(transcript.getvalue())
        receipt['original_inputs_unchanged'] = all(sha(Path(p)) == digest for p, digest in pins.items())
        (out / 'fixture-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if not receipt['original_inputs_unchanged']:
        raise RuntimeError('original input race; never behavioral evidence')
    executable = out / 'build/runtime-schema2'
    receipt['executable_sha256'] = sha(executable)
    (out / 'fixture-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'PS3DK_PROF_RUNTIME_SCHEMA2_PREFLIGHT={executable}', flush=True)


if __name__ == '__main__':
    main()
