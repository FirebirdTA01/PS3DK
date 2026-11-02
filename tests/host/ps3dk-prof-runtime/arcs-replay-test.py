#!/usr/bin/env python3
"""Bounded replay failure cases and independent whole-body CRC oracle."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import zlib

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
p.add_argument('--sanitize', action='store_true')
p.add_argument('--red-stub', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
commands = []


def run(label, argv):
    r = subprocess.run([str(v) for v in argv], capture_output=True, timeout=120,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1'))
    (out / (label + '.stdout')).write_bytes(r.stdout)
    (out / (label + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(label=label, argv=[str(v) for v in argv], rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    return r


flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
core = root / 'sdk/libps3dkprof/src/core'
cc = os.environ.get('CC', 'cc')
obj = out / 'stream.o'
# The symbol check holds the object to the target build: the SDK never enables the stack protector, but a distro
# host compiler may by default (Ubuntu's gcc does), which would add __stack_chk_fail.  Sanitized runs keep the default.
nsp = [] if a.sanitize else ['-fno-stack-protector']
r = run('compile', [cc, *flags, *nsp, '-I', core, '-c', core / 'prof_arcs_stream.c', '-o', obj])
assert r.returncode == 0, r.stderr.decode()
if not a.sanitize:
    r = run('symbols', ['nm', '-u', obj])
    assert r.returncode == 0
    assert all(v.split()[-1] in ['memcpy', 'memset'] for v in r.stdout.decode().splitlines()), r.stdout.decode()
exe = out / 'replay'
extra = [a.red_stub.resolve()] if a.red_stub else []
r = run('link', [cc, *flags, '-I', core, Path(__file__).with_name('arcs_replay_test.c'), obj, *extra, '-o', exe])
assert r.returncode == 0, r.stderr.decode()
cases = ['empty', 'normal', 'short', 'large', 'reentrant', 'truncated', 'extra', 'corrupt',
         'read-errors', 'overcount', 'output-errors', 'state', 'arguments']
passed = 0
checkers = 0
for name in cases:
    r = run(name, [exe, name, out])
    ok = r.returncode == 0
    if ok and name in ['empty', 'normal', 'short', 'large', 'reentrant']:
        data = (out / (name + '.body')).read_bytes()
        m = re.search(rb'bytes=(\d+) crc=(\d+)', r.stdout)
        ok = m is not None and int(m[1]) == len(data) and int(m[2]) == zlib.crc32(data)
        checkers += ok
    passed += ok
    print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
print(f'replay tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
print(f'independent CRC checkers tests=5 pass={checkers} fail={5-checkers}', flush=True)
raise SystemExit(passed != len(cases))
