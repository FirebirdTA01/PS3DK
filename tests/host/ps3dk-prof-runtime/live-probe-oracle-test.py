#!/usr/bin/env python3
"""The live-read probe's oracle with a fake file (host; plain and sanitized)."""
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
src = Path(__file__).parent / 'live_probe_oracle_test.c'
inc = root / 'samples/profiling/live-read-probe/source'
failed = 0
for label, extra in [('plain', []), ('sanitize', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    exe = out / f'oracle-{label}'
    r = subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', *extra,
                        '-I', str(inc), str(src), '-o', str(exe)], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f'{label} compile failed:\n{r.stderr}')
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    ok = r.returncode == 0 and 'live-probe-oracle: PASS' in r.stdout
    failed += not ok
    print(f'{label}: {"PASS" if ok else "FAIL " + r.stderr.strip()}')
raise SystemExit(1 if failed else 0)
