#!/usr/bin/env python3
"""The public live API (ps3dk_prof_live_start/stop) over the real runtime, the live agent and the host OS shim:
every refusal, the shutdown-claim interval, a full stream checked against the file, a cancelling stop (host; plain
and sanitized, one process per case).

The sanitized build does not recover from UBSan findings; a case passes only with rc 0, its PASS line and no
sanitizer report in stderr."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path, help='a new directory for the builds')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
src = root / 'sdk/libps3dkprof/src'
here = Path(__file__).parent
sources = [src / 'core/prof_core.c', src / 'core/prof_drain.c', src / 'pal/pal_host.c', src / 'live/live_agent.c',
           src / 'live/live_os_host.c', src / 'live/live_api.c', here / 'live_api_test.c']
cc = os.environ.get('CC', 'cc')
base = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST',
        '-DPS3DK_PROF_TEST_HOOKS', '-I', str(inc.parent), '-I', str(src / 'core'), '-I', str(src / 'pal'),
        '-I', str(src / 'live')]
# local-exec TLS: see arcs-lifecycle-test.py (the host gcc's ASan + UBSan null check on a TLS address is unsound
# under ld's initial-exec relaxation); every TLS object is defined in this executable.
san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
       '-ftls-model=local-exec']
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', ASAN_OPTIONS='detect_leaks=1')
REPORTS = ['runtime error:', 'ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'SUMMARY: UndefinedBehaviorSanitizer',
           'SUMMARY: AddressSanitizer']

failed = 0
for label, extra in [('plain', []), ('sanitize', san)]:
    exe = out / f'live-api-{label}'
    r = subprocess.run([cc, *base, *extra, *map(str, sources), '-o', str(exe)], capture_output=True, text=True)
    (out / f'{label}.compile.log').write_text(r.stderr)
    if r.returncode != 0:
        raise SystemExit(f'{label} compile failed:\n{r.stderr}')
    cases = subprocess.run([str(exe), '--list'], capture_output=True, text=True, check=True).stdout.split()
    assert cases, 'no cases listed'
    passed = 0
    for name in cases:
        d = out / f'{label}-{name}'
        d.mkdir()
        r = subprocess.run([str(exe), name, str(d)], capture_output=True, text=True, timeout=60, env=env)
        (d / 'stdout').write_text(r.stdout)
        (d / 'stderr').write_text(r.stderr)
        reports = [x for x in REPORTS if x in r.stderr]
        ok = r.returncode == 0 and f'PASS {name}' in r.stdout and not reports
        passed += ok
        for line in r.stdout.splitlines():
            if line.startswith('MEASURE'):
                print(f'{label} {name}: {line}')
        if not ok:
            print(f'FAIL {label} {name} rc={r.returncode} reports={reports} {r.stderr.strip()[:400]}')
    failed += passed != len(cases)
    print(f'live-api {label}: {passed}/{len(cases)} passed')
raise SystemExit(1 if failed else 0)
