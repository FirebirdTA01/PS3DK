#!/usr/bin/env python3
"""samples/gcm/hello-ppu-prof-auto-verdicts refuses at configure, with a message naming -DPS3_PROF_AUTO=ON, when
PS3_PROF_AUTO is off (unset, OFF, or 0). With PS3_PROF_AUTO=ON the refusal does not fire (the configure then goes on
and may stop later for want of the PPU toolchain, which this host test does not use). Host compiler, configure only.

usage: prof-auto-verdicts-configure-test.py <new output dir>"""
import argparse
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
a = p.parse_args()
ROOT = Path(__file__).resolve().parents[2]
SAMPLE = ROOT/'samples/gcm/hello-ppu-prof-auto-verdicts'
OUT = a.out.resolve()
OUT.mkdir(parents=True, exist_ok=False)
cmake = shutil.which('cmake') or 'cmake'
REFUSAL = 'hello-ppu-prof-auto-verdicts needs automatic RSX observation: configure it with -DPS3_PROF_AUTO=ON'

CASES = {'unset': ([], True), 'off': (['-DPS3_PROF_AUTO=OFF'], True), 'zero': (['-DPS3_PROF_AUTO=0'], True),
         'on': (['-DPS3_PROF_AUTO=ON'], False)}
failed = 0
for name, (opts, refuse) in CASES.items():
    cmd = [cmake, '-G', 'Unix Makefiles', '-S', str(SAMPLE), '-B', str(OUT/name), *opts]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    log = r.stdout+r.stderr
    (OUT/(name+'.log')).write_text(log, encoding='utf-8')
    # cmake's message wraps long lines, so compare with whitespace collapsed
    flat = ' '.join(log.split())
    if refuse:
        ok = r.returncode == 1 and REFUSAL in flat and 'PS3_PROF_AUTO is off in this build directory.' in flat
    else:
        ok = REFUSAL not in flat
    print(f'{"PASS" if ok else "FAIL"} {name} rc={r.returncode}')
    if not ok:
        print(log[-2000:])
    failed += not ok
print(f'prof-auto-verdicts configure: {len(CASES) - failed}/{len(CASES)} passed')
raise SystemExit(1 if failed else 0)
