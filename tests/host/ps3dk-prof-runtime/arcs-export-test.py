#!/usr/bin/env python3
"""Real runtime -> stream -> final coordinator, production reader/pair oracles."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
p.add_argument('--reader', required=True, type=Path)
p.add_argument('--sanitize', action='store_true')
p.add_argument('--red-stub', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
here = Path(__file__).parent
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
src = root / 'sdk/libps3dkprof/src'
commands = []


def run(label, argv):
    r = subprocess.run([str(v) for v in argv], capture_output=True, timeout=120,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1'))
    (out / (label + '.stdout')).write_bytes(r.stdout)
    (out / (label + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(label=label, argv=[str(v) for v in argv], rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    return r


flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
         '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_WITH_ARCS=1', '-DPS3DK_PROF_TEST_HOOKS',
         '-I', inc.parent, '-I', src / 'core']
if a.sanitize:
    # local-exec TLS: the host gcc's -O2 ASan + UBSan null check on a TLS address branches on the flags of an
    # initial-exec `add`, which ld relaxes to a flag-less `lea` in an executable, so the branch reads stale flags and
    # reports a false null (gcc 12.4 and 13.3 with binutils 2.42).  Every TLS object here is defined in this executable,
    # so local-exec is valid and leaves nothing for ld to relax; the sanitizers and the null check stay on.
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-ftls-model=local-exec']
files = [src / s for s in ['core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c',
                           'hooks/arcs.c', 'core/prof_arcs_stream.c']]
files.append(a.red_stub.resolve() if a.red_stub else src / 'core/prof_arcs_export.c')
exe = out / 'export'
r = run('compile', [os.environ.get('CC', 'cc'), *flags, *files, here / 'arcs_export_test.c', '-o', exe])
assert r.returncode == 0, r.stderr.decode()
cases = ['normal', 'thread', 'reuse', 'empty', 'short', 'before', 'unregistered', 'unknown', 'acked', 'no-runtime',
         'timing-write', 'timing-flush', 'timing-close', 'join', 'output-header', 'output-body',
         'output-trailer', 'output-flush', 'output-close', 'corrupt', 'truncated', 'extra',
         'mismatch', 'overflow', 'sequence', 'invalid', 'reentrant']
passed = 0
for name in cases:
    r = run(name, [exe, name, out])
    ok = r.returncode == 0
    passed += ok
    print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
print(f'export scenarios tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
if passed != len(cases) or a.red_stub:
    raise SystemExit(passed != len(cases))
pair = out / 'pair-oracle'
target = a.reader.resolve().parent
r = run('pair-compile', ['rustc', '--edition=2021', here / 'arcs_pair_oracle.rs',
                         '--extern', f'ps3dk_prof={target}/libps3dk_prof.rlib',
                         '-L', f'dependency={target}/deps', '-o', pair])
assert r.returncode == 0, r.stderr.decode()
paired = ['normal', 'thread', 'reuse', 'empty', 'short', 'before', 'invalid', 'reentrant', 'acked',
          'unregistered', 'unknown', 'output-flush', 'output-close']
passed = 0
for name in paired:
    arcs = out / (name + '.arcs')
    prof = out / (name + '.prof')
    for cmd, path in [('check-arcs', arcs), ('check', prof)]:
        r = run(name + '-' + cmd, [a.reader, cmd, path])
        assert r.returncode == (1 if name == 'unknown' else 0), r.stdout + r.stderr
    r = run(name + '-pair', [pair, arcs, prof])
    wanted = 1 if name in ['unregistered', 'unknown'] else 0
    ok = r.returncode == wanted
    if name == 'unregistered':
        ok = ok and b'unregistered' in r.stderr
    # Independently decode bound prefix, rather than trusting emitter metadata.
    data = arcs.read_bytes()
    pos = 32
    for _ in range(6):
        n, = struct.unpack_from('<H', data, pos)
        pos += 2 + n
    length, seq, crc = struct.unpack_from('<QQI', data, pos + 20)
    timing = prof.read_bytes()
    offset, last = 16, None
    while offset < len(timing):
        _, n, last = struct.unpack_from('<IIQ', timing, offset)
        offset += 20 + n
    ok = ok and length == len(timing) and seq == last and crc == zlib.crc32(timing) and offset == len(timing)
    passed += ok
    print(f'pair/binding {name}: {"PASS" if ok else "FAIL"}', flush=True)
print(f'pair/binding tests={len(paired)} pass={passed} fail={len(paired)-passed}', flush=True)
assert passed == len(paired)
bad = ['output-header', 'output-body', 'output-trailer', 'corrupt', 'truncated', 'extra']
passed = 0
for name in bad:
    r = run(name + '-reader', [a.reader, 'check-arcs', out / (name + '.arcs')])
    passed += r.returncode in [1, 2]
print(f'failed-output reader tests={len(bad)} pass={passed} fail={len(bad)-passed}', flush=True)
raise SystemExit(passed != len(bad))
