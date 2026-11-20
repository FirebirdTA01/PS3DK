#!/usr/bin/env python3
"""SPU drain work-budget test plus byte-identity receipts. No wall-clock gate.
Usage: drain-cost-test.py OUT (new directory).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
spu = here.parents[2] / 'sdk/libps3dkprof/src/spu'
source = (spu / 'psp_ppu.c').read_text()
anchor = 'static int flush_loss(psp_ppu *p, psp_lane *l, walk *w)\n{'
if source.count(anchor) != 1:
    raise SystemExit('cost instrumentation drift')
instrumented = source.replace(anchor, anchor + '\n    extern uint64_t prof_test_empty_loss_flushes;'
                              '\n    if (!w->pl_count) ++prof_test_empty_loss_flushes;')
core = out / 'instrumented.c'
core.write_text(instrumented)
exe = out / 'drain-cost'
cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
       f'-I{spu}', f'-I{here}', str(core), str(spu / 'psp_spu.c'), str(here / 'model.c'),
       str(here / 'order_model.c'), str(here / 'drain_cost.c'), '-o', str(exe)]
r = subprocess.run(cmd, capture_output=True, text=True)
(out / 'build.log').write_text(r.stdout + r.stderr)
if r.returncode:
    raise SystemExit('build failed; not a cost result')
r = subprocess.run([str(exe), '--cost-gate'], capture_output=True, text=True, timeout=120)
(out / 'results.jsonl').write_text(r.stdout)
(out / 'stderr').write_text(r.stderr)
(out / 'rc').write_text(str(r.returncode) + '\n')
rows = [json.loads(line) for line in r.stdout.splitlines()]


def fault_report(rc):
    # A signal leaves nothing on stderr: name the faulting site from the same build under gdb (when present) and
    # under ASan/UBSan, so a fault seen only on one host is diagnosable from the job log alone.
    lines = [f'drain-cost gate died by signal {-rc} after {len(rows)} of 8 rows']
    # the gate's own fault trace (drain_cost.c): the faulting address and frames, the binary's offsets resolved
    trace = [x for x in r.stderr.splitlines() if x.startswith('DRAIN_COST_FAULT') or '(+0x' in x or '[0x' in x]
    lines += trace[:50]
    offs = [x.split('(+', 1)[1].split(')', 1)[0] for x in trace if str(exe) in x and '(+0x' in x]
    if offs and shutil.which('addr2line'):
        a2l = subprocess.run(['addr2line', '-f', '-C', '-i', '-p', '-e', str(exe), *offs], capture_output=True,
                             text=True, timeout=60)
        lines += ['addr2line:'] + a2l.stdout.splitlines()[:60]
    if shutil.which('gdb'):
        g = subprocess.run(['gdb', '-q', '-batch', '-ex', 'run', '-ex', 'bt 24', '-ex', 'info registers rip',
                            '--args', str(exe), '--cost-gate'], capture_output=True, text=True, timeout=300)
        (out / 'gdb.log').write_text(g.stdout + g.stderr)
        lines += ['gdb:'] + [x for x in (g.stdout + g.stderr).splitlines() if x.startswith(('#', 'Program', 'rip'))][:30]
    sanexe = out / 'drain-cost-asan-fault'
    b = subprocess.run([*cmd[:-2], '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-o', str(sanexe)], capture_output=True, text=True)
    if not b.returncode:
        f = subprocess.run([str(sanexe), '--cost-gate'], capture_output=True, text=True, timeout=300,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))
        (out / 'asan-fault.log').write_text(f.stdout + f.stderr)
        lines += [f'ASan/UBSan build: rc {f.returncode}'] + f.stderr.splitlines()[:40]
    else:
        lines += ['ASan/UBSan build failed: ' + b.stderr[-1000:]]
    return '\n'.join(lines)


if r.returncode < 0:
    raise SystemExit(fault_report(r.returncode))
if r.returncode or r.stderr or len(rows) != 8:
    raise SystemExit(r.stderr or f'missing work-budget results (rc {r.returncode}, {len(rows)} of 8 rows)')
# Baseline bec5c964c: exact original chunk types, lengths and payloads. Times and
# instrumentation counters are deliberately excluded. Independent checks above
# also check the contents against producer truth (a matching digest is not one).
expected = {
    (1, 0): (137, 8025, '1c0c4b031e2c04c1'),
    (1, 256): (265, 146521, 'a752edb9db4b4b03'),
    (1, 2048): (265, 1064025, '2301fc6c3f48ec73'),
    (1, 4096): (265, 2112601, 'c68d520e4e967f23'),
    (6, 0): (807, 47260, '4c22f272259dfbfb'),
    (6, 256): (1575, 878236, 'd80a32081dc247f4'),
    (6, 2048): (1575, 6383260, 'bd6a994f905ef8e8'),
    (6, 4096): (1575, 12674716, 'f9b3633e975e4c7c'),
}
for row in rows:
    if (row['chunks'], row['bytes'], row['hash']) != expected[(row['lanes'], row['events'])]:
        raise SystemExit(f'capture byte identity changed: {row}')
print('drain-cost: eight populations, independent stream/order checks and work budget PASS')

sanexe = out / 'drain-cost-asan'
san = subprocess.run([*cmd[:-2], '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                      '-fno-omit-frame-pointer', '-o', str(sanexe)], capture_output=True, text=True)
(out / 'asan-build.log').write_text(san.stdout + san.stderr)
if san.returncode:
    raise SystemExit('ASan build failed')
r = subprocess.run([str(sanexe), '--cost-gate'], capture_output=True, text=True, timeout=120,
                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1'))
(out / 'asan.log').write_text(r.stdout + r.stderr)
if r.returncode or r.stderr:
    raise SystemExit('ASan cost/stream check failed')

guard = 'if (w.pl_count && flush_loss(p, l, &w) != 0)'
if instrumented.count(guard) != 1:
    raise SystemExit('empty-loss guard mutant drift')
mutant = out / 'guard-removed.c'
mutant.write_text(instrumented.replace(guard, 'if (flush_loss(p, l, &w) != 0)'))
mutexe = out / 'guard-removed'
mutcmd = [str(mutant) if item == str(core) else item for item in cmd[:-2]]
r = subprocess.run([*mutcmd, '-o', str(mutexe)], capture_output=True, text=True)
(out / 'mutant-build.log').write_text(r.stdout + r.stderr)
if r.returncode:
    raise SystemExit('mutant compile failed, not a catch')
r = subprocess.run([str(mutexe), '--cost-gate'], capture_output=True, text=True, timeout=120)
(out / 'mutant.log').write_text(r.stdout + r.stderr)
if r.returncode != 1 or 'empty loss work scales with events' not in r.stderr:
    raise SystemExit('empty-loss guard mutant not caught')
print('drain-cost: ASan and removed-guard mutant PASS')
