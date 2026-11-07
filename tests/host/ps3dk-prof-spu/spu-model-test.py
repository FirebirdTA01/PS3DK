#!/usr/bin/env python3
"""The SPU lane transport host model (design p3.2 r3, slice 1): deterministic traces and the adversarial scheduler.

Builds the portable core (sdk/libps3dkprof/src/spu) with the host model twice, plain and sanitized (ASan + UBSan,
not recoverable).  Each build runs the traces and the scheduler.  Every capture the model writes is then checked by
the UNCHANGED reader: its `check` exit code must equal the trace's expectation; and a test-only checker compiled
against the reader crate compares verdicts, per-source counts, loss ranges and every decoded event with the
producer's ground truth.  Runtime mutants (one change of the depth-zero epoch rule or the dec_write depth refusal
undone each) must fail their named traces.  Model evidence only: no target property is shown here.

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
sources = [spu / 'psp_spu.c', spu / 'psp_ppu.c', here / 'model.c', here / 'order_model.c', here / 'traces.c',
           here / 'order_traces.c', here / 'sched.c']
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

# reader-side state expectations of the order-record controls: `spu-ticks --observations` over each capture that has
# a <name>.ticks-expect ("has TEXT" / "lacks TEXT" lines, each a substring of the whole output)
tpass = tfail = 0
for d in sorted(x for x in out.glob('plain-*') if x.is_dir()):
    for tx in sorted(d.glob('*.ticks-expect')):
        prof = tx.with_suffix('.prof')
        r = subprocess.run([str(a.reader), 'spu-ticks', '--observations', str(prof)], capture_output=True, text=True)
        (out / f'ticks-{tx.stem}.stdout').write_text(r.stdout)
        bad = []
        if r.returncode not in (0, 1, 4):
            bad.append(f'rc {r.returncode}')
        for line in tx.read_text().splitlines():
            if not line:
                continue
            verb, _, text = line.partition(' ')
            if verb not in ('has', 'lacks'):
                bad.append(f'unknown directive {verb!r}')
            elif (text in r.stdout) != (verb == 'has'):
                bad.append(f'{verb} {text!r}')
        if bad:
            tfail += 1
            failed.append(f'spu-ticks {prof.name}: {"; ".join(bad[:4])}')
            print(f'spu-ticks FAIL {prof.name}: {bad[:4]}', flush=True)
        else:
            tpass += 1
print(f'spu-ticks expectations: {tpass} PASS, {tfail} FAIL', flush=True)
if tpass + tfail == 0:
    failed.append('spu-ticks: no capture with an expectation')

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
    # a truth row whose kind does not fit a u8: a checked conversion rejects it instead of wrapping (300 -> 44)
    'kind-out-of-range': lambda t: t.write_text('\n'.join(
        ' '.join(f[:4] + ['300'] + f[5:]) if (f := l.split()) and l[0] != '#' and f[1] == '0' else l
        for l in t.read_text().splitlines()) + '\n'),
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

# runtime revert controls for the depth-zero epoch rule and the dec_write depth refusal: each mutant is one runtime
# file with one change undone; its traces run must fail, and fail on every named trace
mutants = {
    'epoch-always-unknown': ('psp_ppu.c', 'if (!l->known || l->ever_unknown || l->protocol || l->sp != 0)\n'
                             '                    go_unknown(l);', 'go_unknown(l);',
                             ['b1-depth0:', 'b1-depth0-open:', 'b1-pending-end:', 'b1-edepth:']),
    'epoch-ignores-earlier-unknown': ('psp_ppu.c', 'if (!l->known || l->ever_unknown || l->protocol || l->sp != 0)',
                                      'if (!l->known || l->protocol || l->sp != 0)', ['b1-loss:']),
    'no-depth-refusal': ('psp_spu.c', 'return s->depth != 0 || s->suppressed != 0;', 'return (void)s, 0;',
                         ['b1-edepth:', 'b1-suppressed:']),
    'refusal-after-flush': ('psp_spu.c', '    if (dec_depth_refused(s))\n        return PSP_EDEPTH; /* FIRST, before the '
                            'initial flush: nothing flushed, written or published */\n', '',
                            ['b1-edepth:', 'b1-suppressed:']),
}
for name, (fname, old, new, labels) in mutants.items():
    md = out / f'mutant-{name}'
    md.mkdir()
    text = (spu / fname).read_text()
    if text.count(old) != 1:
        failed.append(f'mutant {name}: the change to undo is not in {fname} exactly once')
        print(f'mutant {name}: NOT APPLIED', flush=True)
        continue
    (md / fname).write_text(text.replace(old, new))
    srcs = [md / fname if s.name == fname and s.parent == spu else s for s in sources]
    exe = md / 'spu-model'
    r = run(f'mutant-{name}-compile', [cc, *base, *srcs, '-o', exe])
    if r.returncode != 0:
        failed.append(f'mutant {name}: compile failed')
        print(f'mutant {name}: COMPILE FAILED\n{r.stderr[:1000]}', flush=True)
        continue
    (md / 'traces').mkdir()
    r = run(f'mutant-{name}-traces', [exe, md / 'traces', 'traces'], timeout=3600)
    missed = [x for x in labels if f' {x} ' not in r.stderr]
    verdict = 'KILLED' if r.returncode != 0 and not missed else 'SURVIVED'
    print(f'mutant {name}: {verdict} rc={r.returncode} missed={missed}', flush=True)
    if verdict != 'KILLED':
        failed.append(f'mutant {name} survived (missed {missed})')

print(f'spu-model-test: {"FAIL " + "; ".join(failed) if failed else "PASS"}', flush=True)
raise SystemExit(1 if failed else 0)
