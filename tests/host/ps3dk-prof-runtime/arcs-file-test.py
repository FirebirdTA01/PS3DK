#!/usr/bin/env python3
"""Candidate-file adapter: real files, injected faults, exact ownership."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
p.add_argument('--sanitize', action='store_true')
p.add_argument('--red', action='store_true')
p.add_argument('--reader', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
src = root / 'sdk/libps3dkprof/src'
here = Path(__file__).parent
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
commands = []


def run(label, argv):
    r = subprocess.run([str(x) for x in argv], capture_output=True, timeout=120,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1'))
    (out / (label + '.stdout')).write_bytes(r.stdout)
    (out / (label + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(label=label, argv=[str(x) for x in argv], rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    return r


flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
         '-pthread', '-DPS3DK_PROF_PAL_HOST', '-I', inc.parent, '-I', src / 'core']
if a.sanitize:
    # local-exec TLS: the host gcc's -O2 ASan + UBSan null check on a TLS address branches on the flags of an
    # initial-exec `add`, which ld relaxes to a flag-less `lea` in an executable, so the branch reads stale flags and
    # reports a false null (gcc 12.4 and 13.3 with binutils 2.42).  Every TLS object here is defined in this executable,
    # so local-exec is valid and leaves nothing for ld to relax; the sanitizers and the null check stay on.
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-ftls-model=local-exec']
exe = out / 'file-test'
r = run('compile', [os.environ.get('CC', 'cc'), *flags, src / 'core/prof_arcs_file.c',
                    src / 'pal/pal_host.c', here / 'arcs_file_test.c', '-o', exe])
assert r.returncode == 0, r.stderr.decode()
cases = ['normal', 'empty', 'invalid', 'path-boundary', 'trailing', 'path-boundary-trailing', 'root-path',
         'collision-spool', 'collision-output',
         'open-spool', 'open-output', 'sync-spool', 'sync-output', 'rewind', 'close-spool',
         'close-output', 'both-close', 'cleanup-writing', 'preflight-cleanup', 'reentry', 'pal']
for op in ['spool', 'output', 'read']:
    cases += [op + '-' + kind for kind in ['short', 'over', 'fail', 'eintr8', 'eintr9', 'progress']]
    if op != 'read':
        cases += [op + '-zero']
passed = 0
for name in cases:
    directory = out / name
    directory.mkdir()
    if name in ['path-boundary', 'path-boundary-trailing']:
        length = 1023 - len('/t.arcs.pending')
        while len(str(directory)) < length:
            left = length - len(str(directory))
            directory /= 'a' * min(200, left - 1)
            directory.mkdir()
    directory_arg = str(directory) + ('/' if name in ['trailing', 'path-boundary-trailing'] else '')
    if name == 'root-path':
        directory_arg = '/'  # io_open checks the name and refuses before any OS call.
    r = run(name, [exe, name, directory_arg])
    ok = r.returncode == 0
    passed += ok
    print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
print(f'file scenarios tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
if passed != len(cases) or a.red:
    raise SystemExit(passed != len(cases))
if a.reader:
    exe = out / 'file-integration'
    sources = ['core/prof_core.c', 'core/prof_drain.c', 'pal/pal_host.c', 'hooks/arcs.c',
               'core/prof_arcs_stream.c', 'core/prof_arcs_export.c', 'core/prof_arcs_file.c']
    r = run('integration-compile', [os.environ.get('CC', 'cc'), *flags,
            '-DPS3DK_PROF=1', '-DPS3DK_PROF_WITH_ARCS=1', '-DPS3DK_PROF_TEST_HOOKS',
            *[src / s for s in sources], here / 'arcs_file_integration.c', '-o', exe])
    assert r.returncode == 0, r.stderr.decode()
    cases = ['normal', 'thread', 'reuse', 'unregistered', 'unknown', 'corrupt', 'truncated',
             'extra', 'preflight', 'flush-fail', 'output-close', 'spool-close']
    passed = paired = 0
    for name in cases:
        directory = out / ('integration-' + name)
        directory.mkdir()
        r = run('integration-' + name, [exe, name, directory])
        ok = r.returncode == 0
        passed += ok
        print(f'file integration {name}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok:
            continue
        r = run('pair-' + name, [a.reader, 'check-pair', directory / 'capture.arcs.pending',
                                directory / 'capture.prof'])
        # Partial headers/chunks are incomplete; corrupt body also lacks TRAILER.
        # Empty preflight output is malformed (no magic); corrupted magic in the
        # body reaches the arcs parser as malformed. Pin actual reader codes.
        expected = {'unregistered': 1, 'unknown': 1, 'corrupt': 2,
                    'truncated': 1, 'extra': 1, 'preflight': 2}.get(name, 0)
        pair_ok = r.returncode == expected
        paired += pair_ok
        print(f'file pair {name}: {"PASS" if pair_ok else "FAIL"} rc={r.returncode} expected={expected}', flush=True)
    print(f'file integration tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
    print(f'file pair tests={len(cases)} pass={paired} fail={len(cases)-paired}', flush=True)
    raise SystemExit(passed != len(cases) or paired != len(cases))
