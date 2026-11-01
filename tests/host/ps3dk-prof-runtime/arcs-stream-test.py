#!/usr/bin/env python3
"""Pure body-stream tests and unchanged production writer/reader oracles."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
p.add_argument('--reader', required=True)
p.add_argument('--sanitize', action='store_true')
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
obj = out / 'stream.o'
cc = os.environ.get('CC', 'cc')
r = run('compile', [cc, *flags, '-I', core, '-c', core / 'prof_arcs_stream.c', '-o', obj])
assert r.returncode == 0, r.stderr.decode()
if not a.sanitize:
    r = run('symbols', ['nm', '-u', obj])
    assert r.returncode == 0
    assert all(v.split()[-1] in ['memcpy', 'memset'] for v in r.stdout.decode().splitlines()), r.stdout.decode()
exe = out / 'stream'
r = run('link', [cc, *flags, '-I', core, Path(__file__).with_name('arcs_stream_test.c'),
                 obj, core / 'prof_arcs_writer.c', '-o', exe])
assert r.returncode == 0, r.stderr.decode()
cases = ['config', 'reuse', 'mixed', 'duplicate', 'lifetime', 'budget', 'failures',
         'validation', 'transitions', 'limits', 'long', 'duplicate-key', 'names']
passed = 0
for name in cases:
    r = run(name, [exe, name, out])
    ok = r.returncode == 0
    passed += ok
    print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
print(f'stream scenarios tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
if passed != len(cases):
    raise SystemExit(1)
verdicts = dict(empty=0, reuse=0, mixed=1, duplicate=2, lifetime=0, budget=0, name512=0, utf8=0,
                **{'max-exact': 0, 'limit': 1, 'refused': 1, 'duplicate-key': 2})
verdicts.update({f'failure-{i}-{j}': 1 for i in range(1, 4) for j in range(2)})
passed = 0
for name, wanted in verdicts.items():
    r = run(name + '-reader', [a.reader, 'check-arcs', out / (name + '.arcs')])
    ok = r.returncode == wanted
    if name == 'duplicate':
        ok = ok and b'incarnation/slot not available' in r.stdout + r.stderr
    passed += ok
    print(f'oracle {name}: {"PASS" if ok else "FAIL"} rc={r.returncode}', flush=True)
print(f'stream reader tests={len(verdicts)} pass={passed} fail={len(verdicts)-passed}', flush=True)
raise SystemExit(passed != len(verdicts))
