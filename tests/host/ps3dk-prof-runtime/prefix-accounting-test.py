#!/usr/bin/env python3
"""Check accepted timing prefixes and immutable finalization against actual bytes."""
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
if not a.sanitize and not a.red_stub:
    # Removing the candidate guard must leave no collector symbols/references in either core translation unit
    # (manual archives remain independent).  The core finalization record is unconditional: the build without arcs
    # defines its accessor and nothing else of the arcs path.
    disabled = [v for v in flags if v != '-DPS3DK_PROF_WITH_ARCS=1']
    for name in ['prof_core', 'prof_drain']:
        obj = out / (name + '-disabled.o')
        r = run(name + '-disabled', [os.environ.get('CC', 'cc'), *disabled,
                                    '-c', src / 'core' / (name + '.c'), '-o', obj])
        assert r.returncode == 0, r.stderr.decode()
        r = run(name + '-disabled-symbols', ['nm', obj])
        assert r.returncode == 0 and all(s not in r.stdout for s in [b'_arcs', b'_record_arc', b'_crc32_extend'])
        if name == 'prof_drain':
            assert re.search(rb'^[0-9a-f]+ T ps3dk_prof__finalization$', r.stdout, re.M), r.stdout.decode()
    print('disabled linkage tests=2 pass=2 fail=0', flush=True)
import zlib
cases = ['normal', 'short1', 'short7', 'zeros', 'zero-limit', 'overcount', 'partial',
         'trailer-fail', 'flush-fail', 'close-fail', 'thread', 'join-fail', 'late', 'unregistered',
         'length-overflow', 'chunk-overflow', 'eintr', 'observe', 'refused']


def run_arcs():
    files = [src / s for s in ['core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c', 'hooks/arcs.c', 'core/prof_arcs_writer.c']]
    if a.red_stub:
        files.append(a.red_stub.resolve())
    exe = out / 'prefix'
    r = run('compile', [os.environ.get('CC', 'cc'), *flags, *files,
                        Path(__file__).with_name('prefix_accounting_test.c'), '-o', exe])
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
    passed = 0
    for name in cases:
        capture = out / (name + '.prof')
        prefix = ['setarch', 'x86_64', '-R'] if a.sanitize == 'thread' else []
        r = run(name, [*prefix, exe, name, capture])
        ok = r.returncode == 0
        if ok:
            meta = {k.decode(): int(v) for k, v in re.findall(rb'(\w+)=(\d+)', r.stdout)}
            blob = capture.read_bytes()
            off, chunks, last, trailer = 16, 0, 2**64-1, False
            unreg = None
            while off + 20 <= len(blob):
                kind, size, seq = struct.unpack_from('<IIQ', blob, off)
                end = off + 20 + size
                if end > len(blob):
                    break
                assert zlib.crc32(blob[off:end-4]) == struct.unpack_from('<I', blob, end-4)[0]
                assert seq == chunks
                chunks += 1
                last = seq
                trailer = kind == 6
                if trailer:
                    unreg = struct.unpack_from('<Q', blob, end-20)[0]
                off = end
            ok = (meta['bytes'] == len(blob) and meta['crc'] == zlib.crc32(blob) and
                  meta['chunks'] == chunks and meta['last'] == last and meta['trailer'] == int(trailer))
            if name in ['length-overflow','chunk-overflow']:
                # C asserts the preflight boundary with synthetic near-MAX state;
                # that artificial historical byte/chunk count is not a real file.
                ok = meta['synthetic'] == 1 and meta['io'] == 0 and meta['crc'] == zlib.crc32(blob)
            if trailer:
                ok = ok and meta['unregistered'] == unreg
            expected = 1 if name in ['zero-limit','overcount','partial','late',
                                    'length-overflow','chunk-overflow','eintr'] else 0
            checker = run(name+'-reader', [a.reader, 'check', capture])
            ok = ok and checker.returncode == expected
            if name == 'trailer-fail':
                # The trailer write failed: the C test already required shutdown == ESINKFAIL, and here the record
                # says no trailer and no io finalization.  The half-written trailer is a torn last chunk, so the reader
                # drops it and reads a valid, unfinished file: COMPLETE, exit 0, ending NO-TRAILER.
                ok = ok and meta['trailer'] == 0 and meta['io'] == 0 and not trailer
                ok = ok and b'ending: NO-TRAILER' in checker.stdout and b'torn last chunk dropped' in checker.stdout
        passed += ok
        print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
    print(f'prefix tests={len(cases)} pass={passed} fail={len(cases)-passed}', flush=True)
    return passed == len(cases)


# The core configuration: the record as every shipped archive has it (no WITH_ARCS, no arcs sources).  The C test
# checks bytes, trailer, io, join, result, absence before finalization and no republication; the driver checks the
# bytes against the file and the reader's verdict.
CORE_CASES = [c for c in cases if c != 'chunk-overflow']


def run_core():
    core_flags = [v for v in flags if v != '-DPS3DK_PROF_WITH_ARCS=1']
    core_files = [src / s for s in ['core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c']]
    exe = out / 'prefix-core'
    r = run('core-compile', [os.environ.get('CC', 'cc'), *core_flags, *core_files,
                             Path(__file__).with_name('prefix_accounting_test.c'), '-o', exe])
    assert r.returncode == 0, r.stderr.decode()
    passed = 0
    for name in CORE_CASES:
        capture = out / ('core-' + name + '.prof')
        prefix = ['setarch', 'x86_64', '-R'] if a.sanitize == 'thread' else []
        r = run('core-' + name, [*prefix, exe, name, capture])
        ok = r.returncode == 0
        if ok:
            meta = {k.decode(): int(v) for k, v in re.findall(rb'(\w+)=(-?\d+)', r.stdout)}
            blob = capture.read_bytes()
            off, trailer = 16, False
            while off + 20 <= len(blob):
                kind, size, _ = struct.unpack_from('<IIQ', blob, off)
                end = off + 20 + size
                if end > len(blob):
                    break
                trailer = kind == 6
                off = end
            ok = meta['bytes'] == len(blob) and meta['trailer'] == int(trailer)
            if name == 'length-overflow':
                ok = meta['synthetic'] == 1 and meta['io'] == 0
            expected = 1 if name in ['zero-limit', 'overcount', 'partial', 'late',
                                     'length-overflow', 'eintr'] else 0
            checker = run('core-' + name + '-reader', [a.reader, 'check', capture])
            ok = ok and checker.returncode == expected
            if name == 'trailer-fail':
                # same as the arcs case; result -10 is PS3DK_PROF_ESINKFAIL, the runtime's own verdict on the failure
                ok = ok and meta['trailer'] == 0 and meta['io'] == 0 and meta['result'] == -10 and not trailer
                ok = ok and b'ending: NO-TRAILER' in checker.stdout and b'torn last chunk dropped' in checker.stdout
        passed += ok
        print(f'core {name}: {"PASS" if ok else "FAIL"}', flush=True)
    print(f'prefix core tests={len(CORE_CASES)} pass={passed} fail={len(CORE_CASES)-passed}', flush=True)
    return passed == len(CORE_CASES)


arcs_ok = run_arcs()
core_ok = True if a.red_stub else run_core()   # the red stub replaces an arcs hook: core has nothing to replace
raise SystemExit(not (arcs_ok and core_ok))
