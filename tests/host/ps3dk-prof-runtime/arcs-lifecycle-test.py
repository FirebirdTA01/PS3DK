#!/usr/bin/env python3
"""Exercise arc ownership through the actual runtime. Caller holds a build slot."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
p.add_argument('--reader', required=True)
p.add_argument('--sanitize', choices=['address', 'thread'])
p.add_argument('--red-stub', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
shutil.copyfile(root / 'sdk/include/ps3dk/prof.h', inc / 'prof.h')
src = root / 'sdk/libps3dkprof/src'
commands = []
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1',
           TSAN_OPTIONS='halt_on_error=1:second_deadlock_stack=1:detect_deadlocks=1:exitcode=66')


def run(label, argv):
    r = subprocess.run([str(v) for v in argv], capture_output=True, env=env, timeout=120)
    (out / (label + '.stdout')).write_bytes(r.stdout)
    (out / (label + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(label=label, argv=[str(v) for v in argv], rc=r.returncode,
                         sanitizer_env={k: env[k] for k in ['ASAN_OPTIONS', 'UBSAN_OPTIONS', 'TSAN_OPTIONS']}))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    return r


flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
         '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_WITH_ARCS=1', '-DPS3DK_PROF_TEST_HOOKS',
         '-I', inc.parent, '-I', src / 'core']
if a.sanitize:
    flags += ['-fsanitize=' + ('address,undefined' if a.sanitize == 'address' else 'thread'),
              '-fno-omit-frame-pointer']
    if a.sanitize == 'address':
        # local-exec TLS: the host gcc's -O2 ASan + UBSan null check on a TLS address branches on the flags of an
        # initial-exec `add`, which ld relaxes to a flag-less `lea` in an executable, so the branch reads stale flags
        # and reports a false null (gcc 12.4 and 13.3 with binutils 2.42).  Every TLS object here is defined in this
        # executable, so local-exec is valid and leaves nothing for ld to relax; the sanitizers and the null check stay
        # on.
        flags += ['-ftls-model=local-exec']
files = [src / s for s in ['core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c', 'hooks/arcs.c', 'core/prof_arcs_writer.c']]
if a.red_stub:
    files.append(a.red_stub.resolve())
exe = out / 'lifecycle'
r = run('compile', [os.environ.get('CC', 'cc'), *flags, *files,
                    Path(__file__).with_name('arcs_lifecycle_test.c'), '-o', exe])
assert r.returncode == 0, r.stderr.decode()
if a.sanitize == 'thread':
    # Prove that the same compiler/options/wrapper detects a real race AND
    # accepts its mutex-protected control. Tool failure is never a skip/pass.
    for label, args, wanted in [('race', [], 66), ('locked', ['-DRACE_CTL_LOCKED'], 0)]:
        ctl = out / label
        r = run(label + '-compile', [os.environ.get('CC', 'cc'), *flags, *args,
                                    Path(__file__).with_name('tsan_race_ctl.c'), '-o', ctl])
        assert r.returncode == 0, r.stderr.decode()
        r = run(label + '-control', ['setarch', 'x86_64', '-R', ctl])
        assert r.returncode == wanted, r.stderr.decode()
        if label == 'race':
            assert all(s in r.stderr for s in [b'ThreadSanitizer: data race', b'writer_a', b'writer_b'])
        else:
            assert b'ThreadSanitizer' not in r.stderr
    print('TSan controls tests=2 pass=2 fail=0', flush=True)
if not a.sanitize and not a.red_stub:
    # Removing the candidate guard must leave no collector symbols/references
    # in either core translation unit (manual archives remain independent).
    disabled = [v for v in flags if v != '-DPS3DK_PROF_WITH_ARCS=1']
    for name in ['prof_core', 'prof_drain']:
        obj = out / (name + '-disabled.o')
        r = run(name + '-disabled', [os.environ.get('CC', 'cc'), *disabled,
                                    '-c', src / 'core' / (name + '.c'), '-o', obj])
        assert r.returncode == 0, r.stderr.decode()
        r = run(name + '-disabled-symbols', ['nm', obj])
        assert r.returncode == 0 and b'_arcs' not in r.stdout and b'_record_arc' not in r.stdout
    print('disabled linkage tests=2 pass=2 fail=0', flush=True)
cases = ['config', 'init-fail', 'reuse', 'ack', 'thread', 'paused-retire', 'late', 'raw',
         'consumer-fail', 'finish-fail', 'sink-fail', 'os-id', 'stop-registration', 'limit', 'refused',
         'stale-tls', 'stop-owner']
passed = 0
for name in cases:
    capture = out / (name + '.prof')
    prefix = ['setarch', 'x86_64', '-R'] if a.sanitize == 'thread' else []
    r = run(name, [*prefix, exe, name, capture])
    match = re.search(rb'EXPECT=(\d+)', r.stdout)
    ok = r.returncode == 0 and match is not None
    if ok:
        reader = run(name + '-reader', [a.reader, 'check', capture])
        ok = reader.returncode == int(match[1])
    arc = Path(str(capture) + '.arcs')
    if ok and arc.exists():
        complete = name not in ['late', 'raw', 'limit', 'refused']
        reader = run(name + '-arcs-reader', [a.reader, 'arcs-json', arc])
        ok = reader.returncode == (0 if complete else 1)
        if ok:
            data = json.loads(reader.stdout)
            ok = data['complete'] == complete
            if name in ['reuse', 'stale-tls', 'paused-retire', 'stop-owner']:
                expected_count = 4 if name == 'reuse' else 2 if name == 'stale-tls' else 1
                ok = ok and len(data['finals']) == expected_count
                for inc in range(1, expected_count+1):
                    rows = {(int(v['caller'],16),int(v['callee'],16)): int(v['count'])
                            for v in data['arcs'] if v['incarnation']==inc}
                    ok = ok and rows == {(4,8):10, (8,12):20}
            elif name in ['late', 'raw']:
                ok = ok and data['arcs'] == [] and data['finals'][0]['limit_flags'] == 2
            elif name == 'limit':
                ok = ok and data['arcs'][0]['count'] == str(2**64-1)
            # Check the actual timing trailer against the final consumer's value.
            blob = capture.read_bytes()
            off = 16
            trailer = None
            while off < len(blob):
                kind, size = struct.unpack_from('<II', blob, off)
                if kind == 6:
                    trailer = blob[off + 16:off + 16 + size]
                off += 16 + size + 4
            assert trailer is not None
            reported = int(re.search(rb'unregistered=(\d+)', r.stdout)[1])
            ok = ok and struct.unpack_from('<Q', trailer, len(trailer)-16)[0] == reported
    passed += bool(ok)
    print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
print(f'lifecycle tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
raise SystemExit(passed != len(cases))
