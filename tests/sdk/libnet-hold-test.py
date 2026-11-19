#!/usr/bin/env python3
"""libnet's process-wide network holders (sdk/libnet/src/net_hold.c) on the host: one holder alone makes exactly the
raw calls its front end made before holders existed; several holders share one network that only the last release
finalizes; a concurrent mix keeps every raw call legal.  Plain and sanitized (ASan+UBSan), and a ThreadSanitizer
build of the concurrent case.  usage: libnet-hold-test.py <new output dir>"""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
src = root / 'sdk/libnet/src'
# -no-pie: the pools must sit below 4 GiB, as on the console (the layer refuses a higher pool with EFAULT).
base = [cc, '-std=c11', '-O2', '-g', '-no-pie', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', str(src),
        str(root / 'tests/sdk/libnet-hold-test.c'), str(src / 'net_hold.c'), str(src / 'net_pool.c')]
builds = [('plain', []), ('sanitize', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                       '-fno-omit-frame-pointer']),
          ('tsan', ['-fsanitize=thread'])]
reports = ['runtime error:', 'ERROR: AddressSanitizer', 'WARNING: ThreadSanitizer', 'SUMMARY:']
failed = 0
for label, extra in builds:
    exe = out / f'hold-{label}'
    r = subprocess.run([*base[:1], *extra, *base[1:], '-o', str(exe)], capture_output=True, text=True)
    if r.returncode:
        print(f'FAIL {label} build: {r.stderr[-800:]}')
        failed += 1
        continue
    # TSan needs low ASLR entropy on current kernels (as the profiler's R2/R4a rows)
    pre = ['setarch', 'x86_64', '-R'] if label == 'tsan' else []
    cases = subprocess.run([*pre, str(exe), '--list'], capture_output=True, text=True, check=True).stdout.split()
    if label == 'tsan':
        cases = ['concurrent', 'prof_then_legacy']
    passed = 0
    for name in cases:
        r = subprocess.run([*pre, str(exe), name], capture_output=True, text=True, timeout=120)
        (out / f'{label}-{name}.log').write_text(r.stdout + r.stderr)
        bad = [x for x in reports if x in r.stderr]
        ok = r.returncode == 0 and f'PASS {name}' in r.stdout and not bad
        passed += ok
        if not ok:
            print(f'FAIL {label} {name} rc={r.returncode} {bad} {r.stderr.strip()[:400]}')
    print(f'libnet-hold {label}: {passed}/{len(cases)} passed')
    failed += passed != len(cases)
raise SystemExit(1 if failed else 0)
