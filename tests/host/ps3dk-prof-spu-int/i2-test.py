#!/usr/bin/env python3
"""P3.2 integration slice I2: the lane API, step 3b, the seal and lane lifetime in the real runtime (host PAL), with
real host threads as SPU producers over the HOST EMULATION of the line premises (not target evidence).

Builds i2_test.c with the runtime (core, drain, pal_host, spu/psp_ppu, psp_spu, ext_runtime, psp_host_emul) and
PS3DK_PROF_WITH_SPU in three variants: plain, ASan + UBSan (non-recovering) and TSan.  Every case runs in its own
process in plain and asan.  TSan runs the threaded cases under `setarch x86_64 -R`; its POSITIVE control is
basic-thread-drop-ma (line-A emulation locks removed), which must produce a ThreadSanitizer data-race report, and
every other TSan run must be report-free.  Every capture with an .expect is checked by the reader (exact exit code)
and by the fail-closed checker against the producers' truth.

usage: i2-test.py OUT --reader READER"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('out', type=Path)
p.add_argument('--reader', type=Path, required=True)
a = p.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
lib = root / 'sdk/libps3dkprof/src'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / 'inc'
(inc / 'ps3dk').mkdir(parents=True)
shutil.copy(root / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
cc = os.environ.get('CC', 'cc')
flags = ['-std=c11', '-O1', '-g', '-pthread', '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS',
         '-DPS3DK_PROF_WITH_SPU', '-Wall', '-Wextra', '-Werror', f'-I{inc}', f'-I{lib}/core', f'-I{lib}/pal',
         f'-I{lib}/spu']
srcs = [lib / 'core/prof_core.c', lib / 'core/prof_drain.c', lib / 'pal/pal_host.c', lib / 'spu/psp_ppu.c',
        lib / 'spu/psp_spu.c', lib / 'spu/ext_runtime.c', lib / 'spu/psp_host_emul.c', here / 'i2_test.c']
wrap = '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc'
variants = {'plain': [], 'asan': ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                                  '-fno-omit-frame-pointer'], 'tsan': ['-fsanitize=thread']}
cases = ['basic-thread', 'basic-app', 'seal-live', 'reentry', 'app-other-thread', 'held-sink', 'sink-failure', 'clock',
         'fault-lock', 'fault-unlock-create', 'fault-unlock-stats', 'fault-unlock-image', 'fault-unlock-end',
         'fault-drainer', 'race-1', 'race-2', 'race-3', 'race-4', 'near-wrap-ext', 'near-wrap-v1', 'gen-zero-fresh-slot',
         'threads-sink-failure', 'owner-retired', 'chunk-seq-ext', 'chunk-seq-v1', 'reservation-loss', 'arena-exact',
         'arena-short', 'alloc-lanes', 'alloc-lanes-control', 'unlock-step3b', 'unlock-seal', 'unlock-end-final',
         'corrupt-slot', 'corrupt-line', 'corrupt-seal', 'emul-collision']
threaded = ['basic-thread', 'basic-app', 'seal-live', 'app-other-thread', 'held-sink', 'race-2', 'race-3',
            'sink-failure', 'emul-collision']
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=0',
           TSAN_OPTIONS='halt_on_error=0 exitcode=66')
failed = []

exes = {}
for v, extra in variants.items():
    exe = out / f'i2-{v}'
    r = subprocess.run([cc, *flags, *extra, *map(str, srcs), '-o', str(exe), wrap], capture_output=True, text=True)
    (out / f'build-{v}.log').write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        raise SystemExit(f'build {v} failed:\n{r.stderr[:3000]}')
    exes[v] = exe


# the off build: the extension macros with PS3DK_PROF undefined and 0, as C and C++, linked without the runtime
for lang, comp, std in [('c', cc, '-std=c11'), ('c++', os.environ.get('CXX', 'c++'), '-std=c++17')]:
    for define in ['-UPS3DK_PROF', '-DPS3DK_PROF=0']:
        tag = f'offbuild-{lang.replace("+", "x")}-{"undef" if define.startswith("-U") else "0"}'
        obj, exe = out / f'{tag}.o', out / tag
        r = subprocess.run([comp, '-x', lang, std, '-O1', '-Wall', '-Wextra', '-Werror', define, f'-I{inc}',
                            f'-I{lib}/spu', '-c', str(here / 'offbuild_test.c'), '-o', str(obj)],
                           capture_output=True, text=True)
        ok = r.returncode == 0
        if ok:
            r = subprocess.run([comp, str(obj), '-o', str(exe)], capture_output=True, text=True)
            ok = r.returncode == 0
        undef = ''
        if ok:
            undef = subprocess.run(['nm', '-u', str(obj)], capture_output=True, text=True).stdout
            rr = subprocess.run([str(exe)], capture_output=True, text=True)
            ok = rr.returncode == 0 and 'OFFBUILD PASS' in rr.stdout and 'ps3dk_prof' not in undef
        print(f'{tag}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok:
            failed.append(tag)
            print(r.stdout[-1500:], r.stderr[-1500:], undef[:1500], flush=True)


def run(v, cs, label):
    d = out / f'{v}-{label}'
    d.mkdir()
    cmd = [str(exes[v]), cs, str(d)]
    if v == 'tsan':
        cmd = ['setarch', 'x86_64', '-R', *cmd]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=300)
    (d / 'stdout').write_text(r.stdout)
    (d / 'stderr').write_text(r.stderr)
    return r, d


for v in ['plain', 'asan']:
    for cs in cases:
        r, d = run(v, cs, cs)
        ok = r.returncode == 0 and f'I2 {cs} PASS' in r.stdout and 'ERROR: AddressSanitizer' not in r.stderr \
            and 'runtime error' not in r.stderr
        info = ' '.join(l for l in r.stdout.splitlines() if l.startswith(('NEAR-WRAP', 'CHUNKSEQ', 'ALLOCS', 'HELD', 'COLLISION')))
        print(f'{v} {cs}: {"PASS" if ok else "FAIL"} {info}', flush=True)
        if not ok:
            failed.append(f'{v} {cs}')
            print(r.stdout[-1500:], r.stderr[-1500:], flush=True)

# TSan: the positive control must be detected; the threaded cases must be report-free
r, d = run('tsan', 'basic-thread-drop-ma', 'control-drop-ma')
detected = 'WARNING: ThreadSanitizer: data race' in r.stderr
print(f'tsan positive control (line-A emulation lock removed): {"DETECTED" if detected else "NOT DETECTED"}',
      flush=True)
if not detected:
    failed.append('tsan positive control not detected')
for cs in threaded:
    r, d = run('tsan', cs, cs)
    clean = r.returncode == 0 and 'ThreadSanitizer' not in r.stderr and f'I2 {cs} PASS' in r.stdout
    print(f'tsan {cs}: {"CLEAN PASS" if clean else "FAIL"}', flush=True)
    if not clean:
        failed.append(f'tsan {cs}')
        print(r.stderr[-2500:], flush=True)

# the reader (exact exit codes) and the checker on every capture with an expectation (plain build)
target = a.reader.resolve().parent
checker = out / 'oracle'
r = subprocess.run(['rustc', '--edition=2021', '-O', str(here.parent / 'ps3dk-prof-spu/spu_model_oracle.rs'),
                    '--extern', f'ps3dk_prof={target}/libps3dk_prof.rlib', '-L', f'dependency={target}/deps', '-o',
                    str(checker)], capture_output=True, text=True)
if r.returncode != 0:
    raise SystemExit(f'oracle build failed:\n{r.stderr[:2000]}')
coll = out / 'captures'
coll.mkdir()
n = 0
for prof in sorted(out.glob('plain-*/*.prof')):
    exp = prof.with_suffix('.expect')
    if not exp.exists():
        continue
    for ext in ('prof', 'expect', 'truth'):
        src = prof.with_suffix('.' + ext)
        if src.exists():
            shutil.copy(src, coll / src.name)
    want = [l.split()[1] for l in exp.read_text().splitlines() if l.startswith('exit ')]
    rr = subprocess.run([str(a.reader), 'check', str(prof)], capture_output=True, text=True)
    n += 1
    ok = bool(want) and str(rr.returncode) == want[0]
    print(f'reader {prof.name}: rc {rr.returncode} want {want}: {"PASS" if ok else "FAIL"}', flush=True)
    if not ok:
        failed.append(f'reader {prof.name}')
        print(rr.stdout[:1500], flush=True)
r = subprocess.run([str(checker), str(coll)], capture_output=True, text=True)
print(f'oracle: rc {r.returncode} {r.stdout.strip().splitlines()[-1] if r.stdout else ""}', flush=True)
if r.returncode != 0:
    failed.append('oracle')
    print(r.stdout[:3000], flush=True)
print(f'reader checks: {n} captures', flush=True)
print(f'i2-test: {"FAIL " + "; ".join(failed) if failed else "PASS"}', flush=True)
raise SystemExit(1 if failed else 0)
