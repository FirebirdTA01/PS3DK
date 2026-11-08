#!/usr/bin/env python3
"""The live link OS seam's host shim: errno normalization, bounded EINTR retry, short sends, a send to a closed
peer without SIGPIPE, poll status bits, and positioned file reads (host; plain and sanitized).

The sanitized build does not recover from UBSan findings (-fno-sanitize-recover=undefined, halt_on_error=1), and a
run passes only with rc 0, the PASS marker and no sanitizer report in stderr.  Two undefined-behaviour controls prove
the driver rejects UB: one built with the driver's own flags (it must be stopped), and one built recoverable that
still prints the marker and exits 0 (only the stderr check can reject it).  Either control judged PASS fails the
driver."""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path, help='a new directory for the builds')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
live = root / 'sdk/libps3dkprof/src/live'
src = [live / 'live_os_host.c', Path(__file__).parent / 'live_os_test.c']
cc = os.environ.get('CC', 'cc')
base = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror']
san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', ASAN_OPTIONS='detect_leaks=1')
REPORTS = ['runtime error:', 'ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'SUMMARY: UndefinedBehaviorSanitizer',
           'SUMMARY: AddressSanitizer']


def judge(rc, stdout, stderr):
    """PASS only for rc 0, the marker, and no sanitizer report."""
    reports = [r for r in REPORTS if r in stderr]
    if rc != 0:
        return f'FAIL rc={rc} {stderr.strip()[:300]}'
    if 'live-os: PASS' not in stdout:
        return 'FAIL no PASS marker'
    if reports:
        return f'FAIL sanitizer report in stderr: {reports}'
    return 'PASS'


def build_run(label, flags, sources, args, run_env=env):
    exe = out / label
    r = subprocess.run([cc, *base, *flags, *map(str, sources), '-o', str(exe)], capture_output=True, text=True)
    (out / f'{label}.compile.log').write_text(r.stderr)
    if r.returncode != 0:
        raise SystemExit(f'{label} compile failed:\n{r.stderr}')
    r = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=60, env=run_env)
    (out / f'{label}.stdout').write_text(r.stdout)
    (out / f'{label}.stderr').write_text(r.stderr)
    return judge(r.returncode, r.stdout, r.stderr)


failed = 0
hooks = ['-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-I', str(live)]
for label, extra in [('plain', []), ('sanitize', san)]:
    scratch = out / f'scratch-{label}'
    scratch.mkdir()
    verdict = build_run(f'live-os-{label}', [*hooks, *extra], src, [str(scratch)])
    failed += verdict != 'PASS'
    print(f'{label}: {verdict}')

# Undefined-behaviour controls: a signed overflow the optimizer cannot fold, then the PASS marker and exit 0.
control = out / 'ub_control.c'
control.write_text('#include <limits.h>\n#include <stdio.h>\n'
                   'int main(int argc, char **argv) {\n'
                   '    (void)argv;\n'
                   '    volatile int x = INT_MAX - 1 + argc;\n'
                   '    int y = x + 1;\n'
                   '    printf("%d\\n", y);\n'
                   '    puts("live-os: PASS");\n'
                   '    return 0;\n'
                   '}\n')
# Each control must be rejected for its own reason: the halting one by its exit status, the recovering one (which
# exits 0 with the marker) only by the stderr report check.
recovering = ['-fsanitize=address,undefined', '-fsanitize-recover=undefined', '-fno-omit-frame-pointer']
for label, flags, run_env, reason in [
        ('control-halting', san, env, 'FAIL rc='),
        ('control-recovering', recovering, dict(env, UBSAN_OPTIONS='halt_on_error=0'), 'FAIL sanitizer report')]:
    verdict = build_run(label, flags, [control], [], run_env)
    ok = verdict.startswith(reason)
    failed += not ok
    print(f'{label}: {"REJECTED (" + verdict + ")" if ok else "NOT REJECTED AS EXPECTED: " + verdict}')
raise SystemExit(1 if failed else 0)
