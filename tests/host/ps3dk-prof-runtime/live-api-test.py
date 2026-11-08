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
sources = [src / 'core/prof_core.c', src / 'core/prof_drain.c', src / 'core/prof_version.c', src / 'pal/pal_host.c', src / 'live/live_agent.c',
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

# ps3dk_prof_live_stats from C and C++, enabled and off.  The enabled builds link the runtime objects (compiled as C);
# the off builds link nothing of the profiler, so a reference the off macro leaked would fail the link.
cxx = os.environ.get('CXX', 'c++')
hdr = here / 'live_stats_header.c'
objs = []
for s in sources[:-1]:
    o = out / f'hdr-{s.stem}.o'
    r = subprocess.run([cc, *base, '-c', str(s), '-o', str(o)], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f'runtime object {s.name} failed:\n{r.stderr}')
    objs.append(str(o))
cflags = ['-O2', '-Wall', '-Wextra', '-Werror', '-I', str(inc.parent)]
variants = [
    ('c-enabled', [cc, '-std=c11', *cflags, '-DPS3DK_PROF=1', '-x', 'c', str(hdr), '-x', 'none', *objs, '-pthread']),
    ('cxx-enabled', [cxx, '-std=c++17', *cflags, '-DPS3DK_PROF=1', '-x', 'c++', str(hdr), '-x', 'none', *objs,
                     '-pthread']),
    ('c-off', [cc, '-std=c11', *cflags, '-x', 'c', str(hdr)]),
    ('cxx-off', [cxx, '-std=c++17', *cflags, '-x', 'c++', str(hdr)]),
]
hpass = 0
for label, cmd in variants:
    exe = out / f'live-stats-header-{label}'
    r = subprocess.run([*cmd, '-o', str(exe)], capture_output=True, text=True)
    (out / f'header-{label}.compile.log').write_text(r.stderr)
    if r.returncode != 0:
        print(f'FAIL header {label}: build rc={r.returncode} {r.stderr.strip()[:400]}')
        continue
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    ok = r.returncode == 0 and r.stdout.strip() == 'PASS' and not r.stderr
    hpass += ok
    if not ok:
        print(f'FAIL header {label}: rc={r.returncode} {r.stdout.strip()} {r.stderr.strip()[:200]}')
print(f'live-api header: {hpass}/{len(variants)} passed')
failed += hpass != len(variants)
raise SystemExit(1 if failed else 0)
