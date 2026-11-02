#!/usr/bin/env python3
"""Synthetic probe files with exact expected analyzer outcomes: exit code and diagnostic lines."""
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ANALYZE = Path(__file__).with_name('analyze.py')
HEADER = 'kind,phase,index,tb0,tb1,value,note'
HZ = 79800000
META = f'meta,0,0,{HZ},0,0,'
CLEAN = 'cleanup,0,0,0,0,0,ok'
failures = 0


def spu(phase, n, dec0, t=10 ** 10, jump_at=None, jump_to=None, dec2=True, seed=1):
    rnd = random.Random(seed + phase)
    rows = []
    for i in range(n):
        t += 800000
        w = rnd.randint(2000, 6000)
        tr = t + rnd.randint(0, w)
        if jump_at is not None and i == jump_at:
            dec0 = jump_to + (tr - 10 ** 10)
        dec = (dec0 - (tr - 10 ** 10)) % (1 << 32)
        note = f'dec2={(dec - 4000) % (1 << 32)}' if dec2 else ''
        rows.append(f'spu,{phase},{i},{t},{t + w},{dec},{note}')
    return rows


def rsx(n, t=5 * 10 ** 10):
    return [f'rsx,0,{i},{t + i * 800000},{t + i * 800000 + 40000},{1000 + i * 10000000},' for i in range(n)]


def end(n_spu, n_rsx, note='ok', code=0):
    return f'end,0,0,{n_spu},{n_rsx},{code},{note}'


def run(name, lines, want_rc, want):
    global failures
    with tempfile.NamedTemporaryFile('w', suffix='.csv', delete=False) as f:
        f.write('\n'.join(lines) + '\n')
    r = subprocess.run([sys.executable, str(ANALYZE), f.name], capture_output=True, text=True)
    missing = [w for w in want if w not in r.stdout]
    ok = r.returncode == want_rc and not missing
    failures += not ok
    print(f'{"PASS" if ok else "FAIL"} {name}' + ('' if ok else f' rc={r.returncode} want {want_rc} missing={missing}\n'
                                                   f'{r.stdout}{r.stderr}'))


S = spu(0, 100, 3_000_000_000)
run('valid', [HEADER, META] + S + rsx(50) + [CLEAN, end(100, 50)], 0,
    ['spu-decrementer phase 0: 100 samples in 1 segment(s)', 'mapping UNMEASURED', 'the value changed in 100 of 100',
     'domains: spu USABLE (exploratory); rsx USABLE (exploratory)', 'VERDICT RECORD COMPLETE: spu samples 100'])
run('wrap is a break, never joined', [HEADER, META] + spu(0, 100, 30_000_000) + rsx(30) + [CLEAN, end(100, 30)], 0,
    ['in 2 segment(s)', 'increase: wrap or reset (ambiguous, not joined)'])
run('no rsx samples is reported, not an error', [HEADER, META] + S + [CLEAN, end(100, 0)], 0,
    ['rsx-timestamp: NO SAMPLES'])
run('reset mid-phase', [HEADER, META] + spu(0, 100, 3_000_000_000, jump_at=50, jump_to=4_000_000_000) + rsx(30)
    + [CLEAN, end(100, 30)], 0, ['in 2 segment(s)', 'ambiguous'])
CONST = [f'spu,0,{i},{10 ** 10 + i * 1000},{10 ** 10 + i * 1000 + 50},77,dec2=77' for i in range(30)]
run('constant phase: one summary, not 30 segments', [HEADER, META] + CONST + rsx(30) + [CLEAN, end(30, 30)], 0,
    ['30 samples, CONSTANT value 77: UNMEASURED', 'changed in 0 of 30 samples (no change observed in those reads)',
     'domains: spu CONSTANT; NO CHANGE OBSERVED across the spin'])
run('too short', [HEADER, META] + spu(0, 10, 3_000_000_000) + rsx(30) + [CLEAN, end(10, 30)], 0, ['TOO SHORT to fit'])
bad = list(S)
k, ph, i, t0, t1, v, note = bad[5].split(',')
bad[5] = ','.join([k, ph, i, t1, t0, v, note])
run('invalid bracket is INVALID, not complete', [HEADER, META] + bad + [CLEAN, end(100, 0)], 1,
    ['VERDICT INVALID: line 8: bracket tb1 <= tb0'])
run('truncated record', [HEADER, META] + S, 2, ['VERDICT RECORD INCOMPLETE'])
run('failed run', [HEADER, META] + S[:40] + ['spu-stop,0,40,1,0,41,timeout', CLEAN, end(40, 0, 'spu-start', 99)], 3,
    ['STOPPED after these samples (phase 0 index 40 reason timeout)', 'run failed (spu-start, code 99)'])
run('cleanup failure / left running', [HEADER, META] + S + rsx(30) + ['cleanup,0,0,1,0,4294967276,join',
    end(100, 30)], 3, ['cleanup failed (join, left_running=1)'])
run('rsx unavailable', [HEADER, META, 'rsx-unavailable,0,0,0,0,5,gcm-init'] + S + [CLEAN, end(100, 0)], 3,
    ['rsx-timestamp: UNAVAILABLE (gcm-init)', 'a domain stopped or was unavailable'])
run('duplicate end rows', [HEADER, META] + S + [CLEAN, end(100, 0), end(100, 0)], 1, ['VERDICT INVALID: 2 end rows'])
run('data after end', [HEADER, META] + S + [CLEAN, end(100, 0), 'spu,0,999,1,2,3,'], 1,
    ['VERDICT INVALID: data after the end row'])
run('end counts mismatch', [HEADER, META] + S + [CLEAN, end(99, 0)], 1, ['end row counts spu=99 rsx=0 but the file has'])
run('extra csv field', [HEADER, META, S[0] + ',extra'], 1, ['has 8 fields, want 7'])
run('malformed dec2', [HEADER, META, S[0].rsplit(',', 1)[0] + ',dec2=xyz'], 1, ['malformed dec2'])
run('unexpected note', [HEADER, META, S[0].rsplit(',', 1)[0] + ',hello'], 1, ["unexpected note 'hello'"])
run('decrementer above 32 bits', [HEADER, META, 'spu,0,0,1,2,4294967296,dec2=1'], 1, ['decrementer above 32 bits'])
run('index not increasing', [HEADER, META, S[1], S[0]], 1, ['index not increasing'])
run('wrong columns', ['kind,phase,index,tb0,tb1,value', META], 1, ['VERDICT INVALID: columns'])
run('non-integer field', [HEADER, META, 'spu,0,x,1,2,3,'], 1, ['non-integer field'])
run('meta not first', [HEADER] + S[:1] + [META], 1, ['the first row must be the only meta row'])
run('empty file', [''], 1, ['VERDICT INVALID'])
run('phases are never joined', [HEADER, META] + spu(0, 40, 3_000_000_000) + spu(1, 40, 2_000_000_000, t=2 * 10 ** 10)
    + rsx(30) + [CLEAN, end(80, 30)], 0, ['spu-decrementer phase 0: 40 samples', 'spu-decrementer phase 1: 40 samples'])
# codex 23:38 regressions
run('end code nonzero with note ok is INVALID', [HEADER, META] + S + [CLEAN, end(100, 0, 'ok', 7)], 1,
    ["end note 'ok' contradicts code 7"])
run('end failure note with code 0 is INVALID', [HEADER, META] + S + [CLEAN, end(100, 0, 'spu-start', 0)], 1,
    ["end note 'spu-start' contradicts code 0"])
run('cleanup code nonzero with note ok is INVALID', [HEADER, META] + S + ['cleanup,0,0,0,0,5,ok', end(100, 0)], 1,
    ["cleanup note 'ok' contradicts code 5"])
run('cleanup ok but left running is INVALID', [HEADER, META] + S + ['cleanup,0,0,1,0,0,ok', end(100, 0)], 1,
    ['left_running 1'])
back = list(S)
k, ph, i, t0, t1, v, note = back[10].split(',')
back[10] = ','.join([k, ph, i, str(int(t0) - 10 ** 7), str(int(t0) - 10 ** 7 + 100), v, note])
run('bracket before the previous one ended is INVALID', [HEADER, META] + back + [CLEAN, end(100, 0)], 1,
    ["bracket starts before the previous sample's bracket ended"])
run('sample after cleanup is INVALID', [HEADER, META] + S[:50] + [CLEAN] + S[50:] + [end(100, 0)], 1,
    ['sample after the cleanup row'])
run('sample after its domain stopped is INVALID', [HEADER, META] + S[:50] + ['spu-stop,0,50,1,0,51,timeout'] + S[50:]
    + [CLEAN, end(100, 0, 'spu-start', 1)], 1, ['sample after that domain stopped'])
run('a short nonconstant series is TOO SHORT, not USABLE', [HEADER, META] + spu(0, 10, 3_000_000_000) + rsx(30)
    + [CLEAN, end(10, 30)], 0, ['domains: spu TOO SHORT'])
saw = [f'spu,0,{i},{10 ** 10 + i * 1000},{10 ** 10 + i * 1000 + 50},{100 if i % 2 else 200},dec2=1' for i in range(30)]
run('many one-point segments are NO FITTABLE SEGMENT', [HEADER, META] + saw + rsx(30) + [CLEAN, end(30, 30)], 0,
    ['domains: spu NO FITTABLE SEGMENT'])
frozen_stop = CONST[:25] + ['spu-stop,0,25,1,0,26,timeout']
run('STOPPED and NO CHANGE OBSERVED both survive', [HEADER, META] + frozen_stop + rsx(30)
    + [CLEAN, end(25, 30, 'spu-start', 1)], 3, ['domains: spu CONSTANT; STOPPED; NO CHANGE OBSERVED across the spin'])
print(f'analyze tests: {failures} failed')
sys.exit(1 if failures else 0)
