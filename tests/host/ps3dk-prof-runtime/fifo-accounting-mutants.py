#!/usr/bin/env python3
"""Compile the real portable FIFO accounting controls against guard-removal mutants."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out', type=Path, required=True)
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
header = (root / 'sdk/libps3dkprof/src/fifo/fifo_account.h').read_text()
test = Path(__file__).with_name('fifo-accounting-test.c')
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
mutants = {
    'missing-phase': ('kind != s->phase + 1u || ', ''),
    'owner-token': ('if (s->active && (owner != s->owner || frame != s->frame)) reasons |= FIFO_ACCOUNT_TOKEN;',
                    '/* owner-token guard removed */'),
    'publication-frontier': ('if (next < s->published || next > s->prepared)', 'if (0)'),
    'jump-bytes': ('fifo_account_add(s, &s->prepared, 4);', 'fifo_account_add(s, &s->prepared, 0);'),
}
results = []
for name, edit in [('control', None), *mutants.items()]:
    directory = out / name
    directory.mkdir()
    source = header
    if edit:
        old, new = edit
        assert source.count(old) == 1, (name, old)
        source = source.replace(old, new)
    (directory / 'fifo_account.h').write_text(source)
    exe = directory / 'test'
    command = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I', str(directory), str(test), '-o', str(exe)]
    build = subprocess.run(command, capture_output=True, timeout=60)
    (directory / 'build.stderr').write_bytes(build.stderr)
    assert build.returncode == 0, (name, build.stderr.decode())
    run = subprocess.run([str(exe)], capture_output=True, timeout=30)
    (directory / 'run.stdout').write_bytes(run.stdout)
    (directory / 'run.stderr').write_bytes(run.stderr)
    caught = run.returncode != 0 and b'Assertion' in run.stderr
    assert (run.returncode == 0) if edit is None else caught, (name, run.returncode, run.stderr)
    results.append(dict(name=name, command=command, rc=run.returncode, caught=caught))
    print(name, 'PASS' if edit is None else 'CAUGHT')
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
