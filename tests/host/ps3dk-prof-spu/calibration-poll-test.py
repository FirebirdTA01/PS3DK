#!/usr/bin/env python3
"""Read-only SPU calibration polling: real producer, independent stream/order checks,
and guard-removal mutants. Host model evidence, not PPU/SPU timing evidence.
Usage: calibration-poll-test.py OUT (a new directory).
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('out', type=Path)
args = parser.parse_args()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
root = here.parents[2]
spu = root / 'sdk/libps3dkprof/src/spu'
cc = os.environ.get('CC', 'cc')
flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', f'-I{spu}', f'-I{here}']
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1')
common = [spu / 'psp_spu.c', here / 'model.c', here / 'order_model.c', here / 'calibration_poll.c']


def run(name, core, extra=(), reject=None):
    exe = out / name
    cmd = [cc, *flags, *extra, *map(str, common), str(core), '-o', str(exe)]
    built = subprocess.run(cmd, capture_output=True, text=True)
    (out / f'{name}.build.log').write_text(built.stdout + built.stderr)
    if built.returncode:
        raise SystemExit(f'{name}: build failed (not a killed mutant)')
    result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
    (out / f'{name}.log').write_text(result.stdout + result.stderr)
    (out / f'{name}.rc').write_text(str(result.returncode) + '\n')
    if reject:
        ok = result.returncode == 1 and reject in result.stderr
    else:
        ok = result.returncode == 0 and 'failures=0' in result.stdout and not result.stderr
    print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
    if not ok:
        raise SystemExit(result.stdout + result.stderr)


core = spu / 'psp_ppu.c'
run('plain', core)
run('asan', core, ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer'])
source = core.read_text()
mutants = {
    'answer-id': ('s.cal_answered != l->out_id', '0', 'invalid/unanswered observation'),
    'prefix': ('l->r + i < l->cal_seen_w', '1', 'invalid/unanswered observation'),
    'late-only': ('w.br_tb1 = l->cal_seen_tb;', 'w.br_tb1 = l->tb1;', 'exceeds observation'),
    'release-slots': ('l->cal_seen_w = s.w;',
                      'l->cal_seen_w = s.w; m->st64(m->ctx, l->ea_ctl + PSP_A_READ_IDX, s.w);',
                      'poll consumed, wrote'),
}
for name, (old, new, message) in mutants.items():
    if source.count(old) != 1:
        raise SystemExit(f'{name}: mutant target drift')
    variant = out / f'{name}.c'
    variant.write_text(source.replace(old, new))
    run(name, variant, reject=message)
# Move the timestamp before the accepting load, without altering any validation.
old = 'tb = m->mftb(m->ctx);'
start = 'first = m->ld64(m->ctx, l->ea_ctl + PSP_B_GEN);'
if source.count(old) != 1 or source.count(start) != 1:
    raise SystemExit('early-tb: mutant target drift')
variant = out / 'early-tb.c'
variant.write_text(source.replace(old, '').replace(start, old + '\n        ' + start))
run('early-tb', variant, reject='precedes accepting publication read')
print('calibration-poll: 7 schedules, plain/ASan, 5 mutants PASS')
