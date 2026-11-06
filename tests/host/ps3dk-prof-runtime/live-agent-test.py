#!/usr/bin/env python3
"""Live link agent over loopback: every case in its own empty directory."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path, help='a new directory for the build and per-case scratch')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
src = root / 'sdk/libps3dkprof/src'
here = Path(__file__).parent
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
# Only the profiler header: the SDK's other headers (pthread.h) are PS3-only.
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
commands = []


def run(label, argv, timeout=120):
    r = subprocess.run([str(x) for x in argv], capture_output=True, timeout=timeout,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1'))
    (out / (label + '.stdout')).write_bytes(r.stdout)
    (out / (label + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(label=label, argv=[str(x) for x in argv], rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    return r


flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-DPS3DK_PROF_PAL_HOST',
         '-DPS3DK_PROF_TEST_HOOKS',
         '-I', inc.parent, '-I', src / 'core', '-I', src / 'pal', '-I', src / 'live']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
exe = out / 'live-agent-test'
r = run('compile', [os.environ.get('CC', 'cc'), *flags, src / 'live/live_agent.c', src / 'live/live_os_host.c',
                    src / 'pal/pal_host.c', here / 'live_agent_test.c', '-o', exe])
if r.returncode != 0:
    raise SystemExit('compile failed:\n' + r.stderr.decode())
cases = run('list', [exe, '--list']).stdout.decode().split()
assert cases, 'no cases listed'
failed = []
for name in cases:
    d = out / 'cases' / name
    d.mkdir(parents=True)
    r = run(name, [exe, name, d], timeout=60)
    text = r.stdout.decode()
    for line in text.splitlines():
        if line.startswith('MEASURE'):
            print(name + ': ' + line)
    if r.returncode != 0 or ('PASS ' + name) not in text:
        failed.append(name)
        print('FAIL', name, r.stderr.decode().strip()[:400])
print(f'live-agent: {len(cases) - len(failed)}/{len(cases)} passed')
raise SystemExit(1 if failed else 0)
