#!/usr/bin/env python3
"""The SPU lane transport host model (design p3.2 r3, slice 1): deterministic traces and the adversarial scheduler.

Builds the portable core (sdk/libps3dkprof/src/spu) with the host model twice, plain and sanitized (ASan + UBSan,
not recoverable).  Each build runs the traces and the scheduler.  Every capture the model writes is then checked by
the UNCHANGED reader: its `check` exit code must equal the trace's expectation; and a test-only checker compiled
against the reader crate compares verdicts, per-source counts, loss ranges and every decoded event with the
producer's ground truth.  Model evidence only: no target property is shown here.

usage: spu-model-test.py OUT_DIR --reader PATH/TO/release/ps3dk-prof"""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('out', type=Path, help='a new directory')
p.add_argument('--reader', type=Path, required=True)
a = p.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
spu = root / 'sdk/libps3dkprof/src/spu'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
base = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', f'-I{spu}', f'-I{here}']
san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', ASAN_OPTIONS='detect_leaks=1')
REPORTS = ['runtime error:', 'ERROR: AddressSanitizer', 'ERROR: LeakSanitizer', 'SUMMARY: UndefinedBehaviorSanitizer',
           'SUMMARY: AddressSanitizer']
sources = [spu / 'psp_spu.c', spu / 'psp_ppu.c', here / 'model.c', here / 'traces.c', here / 'sched.c']
failed = []


def run(label, cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    (out / f'{label}.stdout').write_text(r.stdout)
    (out / f'{label}.stderr').write_text(r.stderr)
    return r


for mode, flags in [('plain', []), ('sanitize', san)]:
    exe = out / f'spu-model-{mode}'
    r = run(f'compile-{mode}', [cc, *base, *flags, *sources, '-o', exe])
    if r.returncode != 0:
        raise SystemExit(f'{mode} compile failed:\n{r.stderr}')
    for what, marker in [('traces', 'spu-model: PASS'), ('sched', 'spu-sched: PASS')]:
        d = out / f'{mode}-{what}'
        d.mkdir()
        r = run(f'{mode}-{what}', [exe, d, what], timeout=3600, env=env)
        reports = [x for x in REPORTS if x in r.stderr]
        verdict = 'PASS' if r.returncode == 0 and marker in r.stdout and not reports else 'FAIL'
        print(f'{mode} {what}: {verdict} rc={r.returncode} {r.stdout.strip().splitlines()[-2:] if r.stdout else ""}'
              f'{" sanitizer " + str(reports) if reports else ""}', flush=True)
        if verdict != 'PASS':
            failed.append(f'{mode} {what}')

# pins: the reader CLI and the rlib the checker links, hashed into the receipt
import hashlib
target = a.reader.resolve().parent
rlib = target / 'libps3dk_prof.rlib'
pins = [f'{hashlib.sha256(x.read_bytes()).hexdigest()}  {x}' for x in (a.reader.resolve(), rlib)]
(out / 'pins.txt').write_text('\n'.join(pins) + '\n')
for line in pins:
    print(f'pin {line}', flush=True)

# the unchanged reader on every capture of the plain build: EXACT exit codes for the traces, a separate SMOKE range
# for scheduler samples (their exact verdicts are not established independently)
exact = smoke = 0
for d in sorted(x for x in out.glob('plain-*') if x.is_dir()):
    for prof in sorted(d.glob('*.prof')):
        exp = prof.with_suffix('.expect').read_text().split('\n')
        r = subprocess.run([str(a.reader), 'check', str(prof)], capture_output=True, text=True)
        ex = [l.split()[1:] for l in exp if l.startswith('exit ')]
        sm = [l.split()[1:] for l in exp if l.startswith('smoke-exit ')]
        if len(ex) + len(sm) != 1:
            failed.append(f'reader {prof.name}: no single exit expectation')
            continue
        want = (ex or sm)[0]
        ok = str(r.returncode) == want[0] if ex else str(r.returncode) in want
        if ex:
            exact += 1
        else:
            smoke += 1
        if not ok:
            failed.append(f'reader {prof.name}: rc {r.returncode}, want {want}')
            print(f'reader FAIL {prof.name}: rc {r.returncode} want {want}\n{r.stdout[:600]}', flush=True)
print(f'reader check: {exact} exact-exit captures, {smoke} smoke-exit samples', flush=True)

# the checker against the reader crate
checker = out / 'spu-model-oracle'
r = run('oracle-compile', ['rustc', '--edition=2021', '-O', here / 'spu_model_oracle.rs', '--extern',
                           f'ps3dk_prof={rlib}', '-L', f'dependency={target}/deps', '-o', checker])
if r.returncode != 0:
    raise SystemExit(f'oracle compile failed:\n{r.stderr}')
for d in sorted(x for x in out.glob('plain-*') if x.is_dir()):
    r = run(f'oracle-{d.name}', [checker, d])
    print(f'oracle {d.name}: rc={r.returncode} {r.stdout.strip().splitlines()[-1] if r.stdout else ""}', flush=True)
    if r.returncode != 0:
        failed.append(f'oracle {d.name}')
        print(r.stdout[:2000], flush=True)

# checker negative controls: each must fail (rc != 0) on a corrupted copy of one passing trace
import shutil
src = out / 'plain-traces'
controls = {
    'truth-missing': lambda t: t.unlink(),
    'truth-truncated': lambda t: t.write_text('\n'.join(t.read_text().splitlines()[:-1]) + '\n'),
    'identity-changed': lambda t: t.write_text(t.read_text().replace('\n1 0 ', '\n1 0 9', 1)),
    'unknown-directive': None,
}
for name, damage in controls.items():
    d = out / f'control-{name}'
    d.mkdir()
    for ext in ('prof', 'truth', 'expect'):
        shutil.copy(src / f't-clean.{ext}', d / f't-clean.{ext}')
    if damage:
        damage(d / 't-clean.truth')
    else:
        with open(d / 't-clean.expect', 'a') as f:
            f.write('frobnicate 1\n')
    r = run(f'oracle-control-{name}', [checker, d])
    verdict = 'REJECTED' if r.returncode != 0 else 'ACCEPTED'
    print(f'oracle control {name}: {verdict} rc={r.returncode} {r.stdout.strip().splitlines()[0] if r.stdout else ""}',
          flush=True)
    if r.returncode == 0:
        failed.append(f'oracle control {name} was accepted')

print(f'spu-model-test: {"FAIL " + "; ".join(failed) if failed else "PASS"}', flush=True)
raise SystemExit(1 if failed else 0)
