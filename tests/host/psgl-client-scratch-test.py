#!/usr/bin/env python3
"""Compile the actual scratch allocator against a queued-reader memory backend."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'sdk/libPSGL/src/psgl_context.c')
args = parser.parse_args()
source = args.source.read_text()
start = source.index('static int psgl_alloc_client_array_scratch(')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    if source[end] == '{': depth += 1
    elif source[end] == '}': depth -= 1
    end += 1
with tempfile.TemporaryDirectory(prefix='psgl-client-scratch-') as temporary:
    tmp = Path(temporary)
    (tmp / 'allocator.h').write_text(source[start:end])
    exe = tmp / ('scratch.exe' if os.name == 'nt' else 'scratch')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-I' + str(tmp), str(ROOT / 'tests/host/fixtures/psgl-client-scratch.c'),
                    '-o', str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
