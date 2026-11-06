#!/usr/bin/env python3
"""overhead-bench verifier (Plan A r2 2.3 and 3.3; probe-design-r1 11.2 A-1/A-2/A-3, r2 D7).

usage: ob_verify.py bench CSV [--receipt FILE] [--profile production|test --cases LIST --r N]
       ob_verify.py gpu GPU_CSV [--profile production|test --r N]
       ob_verify.py sample CSV (--arm P0a|P1|P2|P3|P0b | --profile test --workers N)
       every mode: [--manifest-self SHA --client-self SHA]
Exit status: exactly 0 PASS or 1 FAIL (every reason printed).  Nothing here computes or judges a timing figure; the
populations it accepts are printed per (case, TU).

Profiles: production (the default) pins the bench's case inventory (CASES_PRODUCTION, the order of common/ob_drv.c)
and R = 200, the GPU section's R = 200 and the sample's four phases.  test is only for fixtures and the host models:
it needs --cases (bench) and --r explicitly; every other rule is the same.

Every field is typed and ranged (unsigned 64-bit unless stated; decrementer readings and command words 32-bit; rc a
signed 32-bit integer; counted and dec_reset exactly 0 or 1; staged a signed 64-bit integer).  A malformed field is a
FAIL, never a skipped row.

bench: line 1 "# OB seed=S blocks=N digest=D tb_freq=F" (F > 0), the exact column header, N BLK rows, "END,N".
  - the rows are exactly the seeded schedule recomputed here (common/ob_sched.c), N = its length (a complete run),
    index 0..N-1 in order, the digest matches;
  - ledgers equal the plan (common/ob_cases.c) on every row, counted or not: ON timed/cold rows the plan, ON warm
    rows the plan with regs = the case's descriptor count, OFF/OFF-b rows all zero;
  - counted: SPU rows must be counted = 1 (the writer state the SPU reports); PPU rows 0 (target: the counted ledger
    comes from the capture) or 1 (host model);
  - rc: every row 0 (a nonzero rc is a FAIL: S-backpressure's documented flush result is 0 too);
  - PPU rows: ppu_t0, ppu_t1, dec_reset, spu_cmd, spu_echo all 0; t1 < t0 = INVALID (excluded, counted);
  - SPU rows: spu_cmd = (seq << 16) | (case << 8) | tu with seq the next of 1..65535 (identity of the block),
    spu_echo = spu_cmd; Plan A r2 2.3 per row, in this order: ppu_t1 < ppu_t0 REGRESSED; dec_reset != 1
    MISSING-RESET; 2 x (ppu_t1 - ppu_t0) >= 2^32 OVERRUN-POSSIBLE (RMAX = 2 decrementer ticks per timebase tick,
    the scoped premise of Plan B 4.2, strict); else the tick count (t0 - t1) mod 2^32 is valid.  Flagged rows are
    excluded and counted; raw endpoints stay as data;
  - every (case, TU) has at least one accepted timed (or cold) row (non-vacuous);
  - --receipt: the file must exist and be exactly "BENCH_END rows=N invalid=K ledger_mismatches=0
    receipt=complete\\n" (K = the INVALID PPU rows); an empty or absent file is a FAIL.
gpu: "# OBG seed=S blocks=N digest=D", the exact header, N GPU rows (the seeded G schedule), "END,N".  rc 0 or 2
  (WRAP: excluded); anything else FAIL.  cmd_bytes over accepted variant rows: one value per variant, g1 = g0 +
  64 x 48, g2 = g1; calibrate rows 0 bytes; t1 < t0 or r1 < r0 = INVALID (excluded); every G case has an accepted
  timed row.
sample: the heterogeneous-workload CSV: the exact header (27 columns); MEM rows for points 0, 1, 2 in order: items 1
  with digest (the query rc) 0 and 0 < mem_available <= mem_total, or items 0 with UNAVAILABLE in both memory
  fields (reported, and a FAIL: the arm has no memory figure; a number from a failed query is a FAIL too); FRAME rows
  numbered 0..n-1, elapsed fields unsigned or NA, nonflip = frame - flipwait when both are numbers and flipwait <=
  frame, else NA; NA rows excluded and counted; at least one accepted FRAME row; one tb_freq value, nonzero; SPE
  inventory: the expected workers W are bound by --arm (production: every Plan A arm, 6) or --workers (test only);
  every frame of an SPU phase (1, 2) has exactly one SPE row per lane 0..W-1 with its FRAME row's frame, phase
  and sequence; no SPE row on a PPU-only phase (0, 3); no lane outside 0..W-1; no duplicated (frame, sequence,
  lane); the 2.3 rules on spe_dispatch_tb/spe_complete_tb and spu_dec_written per row; every (SPU phase present,
  lane) has an accepted row, else "SPE ticks UNAVAILABLE" for it (a FAIL: that run cannot support the measurement);
  END last: frames = FRAME rows, checked = frames, rc = 0,
  phase mask = the phases of the FRAME rows (production: all four, 15).
--manifest-self and --client-self must be equal when given (the .self hash binding).
"""
import argparse
import csv
import re
import sys

M64 = (1 << 64) - 1
U32 = (1 << 32) - 1
RMAX = 2
CASES = ['P-hot', 'P-nest8', 'P-cold', 'P-prefill', 'P-drop', 'P-frame', 'P-empty', 'S-pair', 'S-flush-0',
         'S-flush-1', 'S-flush-16', 'S-flush-128', 'S-stagefull', 'S-backpressure']
CASES_PRODUCTION = ['P-hot', 'P-nest8', 'P-cold', 'P-drop', 'P-frame', 'P-empty', 'S-pair', 'S-flush-0', 'S-flush-1',
                    'S-flush-16', 'S-flush-128', 'S-stagefull', 'S-backpressure']
N = {'P-hot': 256, 'P-nest8': 32, 'P-cold': 1, 'P-prefill': 4095, 'P-drop': 256, 'P-frame': 256, 'P-empty': 0,
     'S-pair': 64, 'S-flush-0': 0, 'S-flush-1': 1, 'S-flush-16': 16, 'S-flush-128': 128, 'S-stagefull': 64,
     'S-backpressure': 16}
PAIR = 3
FIELDS = ['seq', 'dropped', 'regs', 'depth', 'sseq', 'tail_lost', 'staged', 'published', 'lost']
HEADER = ('kind,index,case,tu,block,rep,t0,t1,seq,dropped,regs,depth,sseq,tail_lost,staged,published,lost,counted,'
          'rc,ppu_t0,ppu_t1,dec_reset,spu_cmd,spu_echo').split(',')
TUS = ['on', 'off', 'off_b']
R = 200
COLD_SITES = 64
WARM_REGS = {'P-hot': 1, 'P-drop': 1, 'P-nest8': 8}
GPU_SALT = 0x4F42475055534544
G_CASES = ['G-g0', 'G-g1', 'G-g2', 'G-cal-idle', 'G-cal-queued']
GPU_HEADER = 'kind,index,case,blk,rep,r0,r1,cmd_bytes,t0,t1,rc'.split(',')
ZONE_BYTES = 48
DRAWS = 64
SAMPLE_HEADER = ('kind,frame,phase,sequence,lane,items,digest,compute_us,frame_us,spu_dec_begin,spu_dec_end,gpu_begin,'
                 'gpu_end,gpu_valid,compute_elapsed_tb,frame_elapsed_tb,submit_elapsed_tb,flipwait_elapsed_tb,'
                 'nonflip_elapsed_tb,drain_elapsed_tb,profcall_elapsed_tb,tb_freq,spu_dec_written,mem_total,'
                 'mem_available,spe_dispatch_tb,spe_complete_tb').split(',')
ELAPSED = ['compute_elapsed_tb', 'frame_elapsed_tb', 'submit_elapsed_tb', 'flipwait_elapsed_tb', 'nonflip_elapsed_tb',
           'drain_elapsed_tb', 'profcall_elapsed_tb']


class Bad(Exception):
    pass


def uint(s, hi=M64, what='field'):
    if not re.fullmatch(r'0|[1-9][0-9]*', s or ''):
        raise Bad(f'{what} {s!r} is not an unsigned integer')
    v = int(s)
    if v > hi:
        raise Bad(f'{what} {v} out of range (max {hi})')
    return v


def sint(s, lo, hi, what='field'):
    if not re.fullmatch(r'-?(0|[1-9][0-9]*)', s or '') or s == '-0':
        raise Bad(f'{what} {s!r} is not an integer')
    v = int(s)
    if not lo <= v <= hi:
        raise Bad(f'{what} {v} out of range')
    return v


def flag(s, what):
    if s not in ('0', '1'):
        raise Bad(f'{what} {s!r} is not 0 or 1')
    return int(s)


def planned(case):
    n = N[case]
    p = dict.fromkeys(FIELDS, 0)
    if case == 'P-hot':
        p.update(seq=PAIR * n, depth=1)
    elif case == 'P-nest8':
        p.update(seq=8 * PAIR * n, depth=8)
    elif case == 'P-cold':
        p.update(seq=PAIR, regs=1, depth=1)
    elif case == 'P-prefill':
        p.update(seq=n, depth=1)
    elif case == 'P-drop':
        p.update(seq=PAIR * n, dropped=PAIR * n, depth=1)
    elif case == 'P-frame':
        p.update(seq=n)
    elif case == 'S-pair':
        p.update(sseq=2 * n, staged=2 * n)
    elif case.startswith('S-flush-'):
        p.update(published=n, staged=-n)
    elif case == 'S-stagefull':
        p.update(sseq=2 * n, tail_lost=2 * n)
    elif case == 'S-backpressure':
        p.update(staged=-n, lost=n)
    return p


class Rng:
    def __init__(self, seed):
        self.x = seed & M64

    def next(self):
        if self.x == 0:
            self.x = 0x9E3779B97F4A7C15
        x = self.x
        x ^= x >> 12
        x ^= (x << 25) & M64
        x ^= x >> 27
        self.x = x
        return (x * 0x2545F4914F6CDD1D) & M64


def shuffle(a, lo, n, rng):
    for i in range(n, 1, -1):
        lim = M64 - M64 % i
        while True:
            v = rng.next()
            if v < lim:
                break
        j = v % i
        a[lo + i - 1], a[lo + j] = a[lo + j], a[lo + i - 1]


def schedule(seed, cases, r=R):
    """cases: [(case index, is_cold)] in order; returns [(case, tu, kind, rep)] as common/ob_sched.c"""
    out = []
    for c, cold in cases:
        if not cold:
            for t in range(3):
                out.append((c, t, 1, 0))
    nwarm = len(out)
    rng = Rng(seed)
    shuffle(out, 0, nwarm, rng)
    for c, cold in cases:
        for t in range(3):
            for i in range(COLD_SITES if cold else r):
                out.append((c, t, 3 if cold else 2, i))
    shuffle(out, nwarm, len(out) - nwarm, rng)
    return out


def gpu_schedule(seed, r=R):
    """as common/ob_sched.c ob_gpu_schedule: [(case, tu, kind, rep)]"""
    out = [(1, 0, 1, 0)]
    for c in range(len(G_CASES)):
        for i in range(r):
            out.append((c, 0, 2, i))
    shuffle(out, 1, len(out) - 1, Rng(seed ^ GPU_SALT))
    return out


def digest(blocks):
    h = 0xCBF29CE484222325
    for b in blocks:
        for v in b:
            for q in range(4):
                h ^= (v >> (8 * q)) & 0xFF
                h = (h * 0x100000001B3) & M64
    return h


def spu_window(ppu_t0, ppu_t1, reset):
    """Plan A r2 2.3: None when the tick count is valid, else the flag"""
    if ppu_t1 < ppu_t0:
        return 'REGRESSED'
    if reset != 1:
        return 'MISSING-RESET'
    if RMAX * (ppu_t1 - ppu_t0) >= 1 << 32:
        return 'OVERRUN-POSSIBLE'
    return None


def read_lines(path):
    lines = open(path, encoding='ascii', newline='').read().split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    if any('\r' in ln for ln in lines):
        raise Bad('CR in the file')
    return lines


def verify_bench(path, receipt, profile, cases, r):
    reasons = []
    lines = read_lines(path)
    m = re.fullmatch(r'# OB seed=(0|[1-9][0-9]*) blocks=(0|[1-9][0-9]*) digest=([0-9a-f]{16}) tb_freq=(0|[1-9][0-9]*)',
                     lines[0] if lines else '')
    if not m:
        return ['header line 1 malformed'], []
    seed, n, dg, freq = uint(m.group(1), what='seed'), int(m.group(2)), int(m.group(3), 16), int(m.group(4))
    if freq == 0:
        reasons.append('tb_freq is 0')
    if len(lines) < 2 or lines[1].split(',') != HEADER:
        return reasons + ['column header differs (a missing or extra column)'], []
    rows = list(csv.reader(lines[2:]))
    if not rows or rows[-1] != ['END', str(n)]:
        reasons.append('END row missing or its count differs from blocks')
    body = rows[:-1] if rows and rows[-1][:1] == ['END'] else rows
    if any(r[:1] != ['BLK'] for r in body):
        reasons.append('a row that is neither BLK nor the last END')
    sched = schedule(seed, [(CASES.index(c), c == 'P-cold') for c in cases], r)
    if len(sched) != n:
        reasons.append(f'blocks={n}, the pinned schedule has {len(sched)} (an incomplete or foreign run)')
    if digest(sched) != dg:
        reasons.append('digest does not match the pinned seeded schedule')
    if len(body) != len(sched):
        reasons.append(f'{len(body)} BLK rows for {len(sched)} scheduled blocks')
    kinds = {1: 'warm', 2: 'timed', 3: 'cold'}
    invalid = 0
    mism = 0
    pops = {}
    flags = {}
    cmd_seq = 0
    for i, row in enumerate(body):
        if len(row) != len(HEADER):
            reasons.append(f'row {i}: {len(row)} fields')
            break
        f = dict(zip(HEADER, row))
        try:
            if f['index'] != str(i):
                raise Bad(f'index {f["index"]} (a duplicated, removed or reordered row)')
            if i >= len(sched):
                raise Bad('beyond the schedule')
            c, t, k, rep = sched[i]
            if (f['case'], f['tu'], f['block'], f['rep']) != (CASES[c], TUS[t], kinds[k], str(rep)):
                raise Bad(f'{f["case"]}/{f["tu"]}/{f["block"]}/{f["rep"]} is not the seeded block '
                          f'{CASES[c]}/{TUS[t]}/{kinds[k]}/{rep} (out of order or missing)')
            spu = f['case'].startswith('S-')
            t0 = uint(f['t0'], U32 if spu else M64, 't0')
            t1 = uint(f['t1'], U32 if spu else M64, 't1')
            got = {}
            for name in FIELDS:
                got[name] = sint(f[name], -(1 << 63), (1 << 63) - 1, name) if name == 'staged' else uint(f[name], what=name)
            counted = flag(f['counted'], 'counted')
            rc = sint(f['rc'], -(1 << 31), (1 << 31) - 1, 'rc')
            pt0, pt1 = uint(f['ppu_t0'], what='ppu_t0'), uint(f['ppu_t1'], what='ppu_t1')
            reset = flag(f['dec_reset'], 'dec_reset')
            cmd, echo = uint(f['spu_cmd'], U32, 'spu_cmd'), uint(f['spu_echo'], U32, 'spu_echo')
        except Bad as e:
            reasons.append(f'row {i}: {e}')
            break
        if rc != 0:
            reasons.append(f'row {i} {f["case"]}/{f["tu"]}: rc {rc} (every block must succeed)')
        want = dict.fromkeys(FIELDS, 0)
        if f['tu'] == 'on':
            want = planned(f['case'])
            if f['block'] == 'warm':
                want['regs'] = WARM_REGS.get(f['case'], 0)
        if got != want:
            mism += 1
            if mism <= 5:
                diff = {x: (got[x], want[x]) for x in FIELDS if got[x] != want[x]}
                reasons.append(f'row {i} {f["case"]}/{f["tu"]}/{f["block"]}: ledger {diff} (got, planned)')
        key = (f['case'], f['tu'])
        if spu:
            if counted != 1:
                reasons.append(f'row {i} {f["case"]}: an SPU row must be counted (counted={counted})')
            cmd_seq = cmd_seq % 65535 + 1
            want_cmd = (cmd_seq << 16) | (CASES.index(f['case']) << 8) | (0 if f['tu'] == 'on' else 1)
            if cmd != want_cmd or echo != cmd:
                reasons.append(f'row {i} {f["case"]}: command {cmd:#x} echo {echo:#x}, want {want_cmd:#x} (identity)')
            fl = spu_window(pt0, pt1, reset)
        else:
            if pt0 or pt1 or reset or cmd or echo:
                reasons.append(f'row {i} {f["case"]}: a PPU row carries SPU fields')
            fl = 'INVALID' if t1 < t0 else None
            invalid += fl == 'INVALID'
        if f['block'] == 'warm':
            continue
        pops.setdefault(key, [0, 0])
        if fl:
            flags[(key, fl)] = flags.get((key, fl), 0) + 1
            pops[key][1] += 1
        else:
            pops[key][0] += 1
    for c in cases:
        for t in TUS:
            acc, exc = pops.get((c, t), [0, 0])
            if acc == 0:
                reasons.append(f'{c}/{t}: no accepted timed block ({exc} excluded): a vacuous population')
    if receipt is not None:
        try:
            rec = open(receipt, encoding='ascii', newline='').read()
        except OSError as e:
            rec = None
            reasons.append(f'receipt: {e}')
        want = f'BENCH_END rows={len(body)} invalid={invalid} ledger_mismatches=0 receipt=complete\n'
        if rec is not None and rec != want:
            reasons.append(f'receipt does not match the CSV (want {want.strip()!r}): {rec.strip()!r}'
                           + (' (empty)' if rec == '' else ''))
    summary = [f'{c}/{t} accepted {pops.get((c, t), [0, 0])[0]} excluded {pops.get((c, t), [0, 0])[1]}'
               for c in cases for t in TUS]
    summary += [f'{k[0][0]}/{k[0][1]} {k[1]} {v}' for k, v in sorted(flags.items())]
    return reasons, summary


def verify_gpu(path, profile, r):
    reasons = []
    lines = read_lines(path)
    m = re.fullmatch(r'# OBG seed=(0|[1-9][0-9]*) blocks=(0|[1-9][0-9]*) digest=([0-9a-f]{16})', lines[0] if lines else '')
    if not m:
        return ['gpu header line 1 malformed'], []
    seed, n, dg = uint(m.group(1), what='seed'), int(m.group(2)), int(m.group(3), 16)
    if len(lines) < 2 or lines[1].split(',') != GPU_HEADER:
        return ['gpu column header differs (a missing or extra column)'], []
    rows = list(csv.reader(lines[2:]))
    if not rows or rows[-1] != ['END', str(n)]:
        reasons.append('END row missing or its count differs from blocks (an early stop)')
    body = rows[:-1] if rows and rows[-1][:1] == ['END'] else rows
    sched = gpu_schedule(seed, r)
    if digest(sched) != dg or len(sched) != n:
        reasons.append('gpu digest or block count does not match the pinned seeded G schedule')
    if len(body) != len(sched):
        reasons.append(f'{len(body)} GPU rows for {len(sched)} scheduled blocks')
    kinds = {1: 'warm', 2: 'timed'}
    size, pops = {}, {}
    for i, row in enumerate(body):
        if len(row) != len(GPU_HEADER) or row[0] != 'GPU':
            reasons.append(f'gpu row {i}: malformed')
            break
        f = dict(zip(GPU_HEADER, row))
        try:
            if f['index'] != str(i):
                raise Bad(f'index {f["index"]} (a duplicated, removed or reordered row)')
            c, _, k, rep = sched[i]
            if (f['case'], f['blk'], f['rep']) != (G_CASES[c], kinds[k], str(rep)):
                raise Bad(f'{f["case"]}/{f["blk"]}/{f["rep"]} is not the seeded block {G_CASES[c]}/{kinds[k]}/{rep}')
            r0, r1 = uint(f['r0'], what='r0'), uint(f['r1'], what='r1')
            b = uint(f['cmd_bytes'], U32, 'cmd_bytes')
            t0, t1 = uint(f['t0'], what='t0'), uint(f['t1'], what='t1')
            rc = uint(f['rc'], 15, 'rc')
        except (Bad, IndexError) as e:
            reasons.append(f'gpu row {i}: {e}')
            break
        if rc not in (0, 2):
            reasons.append(f'gpu row {i} {f["case"]}: rc {rc} (a failed block)')
            continue
        cal = f['case'].startswith('G-cal')
        if cal and b != 0:
            reasons.append(f'gpu row {i}: a calibrate row with cmd_bytes {b}')
        bad = rc == 2 or t1 < t0 or (not cal and r1 < r0)
        if f['blk'] == 'timed':
            pops.setdefault(f['case'], [0, 0])[1 if bad else 0] += 1
        if bad or cal:
            continue
        if size.setdefault(f['case'], b) != b:
            reasons.append(f'gpu row {i}: {f["case"]} cmd_bytes {b}, earlier {size[f["case"]]} (one value per variant)')
    for c in G_CASES:
        if pops.get(c, [0, 0])[0] == 0:
            reasons.append(f'{c}: no accepted timed block (a vacuous population)')
    g0, g1, g2 = size.get('G-g0'), size.get('G-g1'), size.get('G-g2')
    if None not in (g0, g1, g2):
        if g1 - g0 != DRAWS * ZONE_BYTES:
            reasons.append(f'cmd_bytes g1 - g0 = {g1 - g0}, want {DRAWS * ZONE_BYTES}')
        if g2 != g1:
            reasons.append(f'cmd_bytes g2 = {g2}, g1 = {g1} (g2 must match g1)')
    return reasons, [f'{c} accepted {pops.get(c, [0, 0])[0]} excluded {pops.get(c, [0, 0])[1]}' for c in G_CASES]


SPU_PHASES = {1, 2}
ARMS = {'P0a': 6, 'P1': 6, 'P2': 6, 'P3': 6, 'P0b': 6}  # Plan A r2 5.1 arms: WORK_MAX_SPUS workers each (reviewed)


def verify_sample(path, profile, workers):
    reasons = []
    lines = read_lines(path)
    rows = list(csv.reader(lines))
    if not rows or rows[0] != SAMPLE_HEADER:
        return ['sample header differs (a missing or extra column)'], []
    ix = {h: i for i, h in enumerate(SAMPLE_HEADER)}
    frames, spe, mem = [], [], []
    end = None
    for k, r in enumerate(rows[1:], 1):
        if len(r) != len(SAMPLE_HEADER):
            reasons.append(f'line {k + 1}: {len(r)} fields')
            return reasons, []
        if end is not None:
            reasons.append('a row after END')
            break
        {'FRAME': frames, 'SPE': spe, 'MEM': mem}.get(r[0], []).append(r)
        if r[0] == 'END':
            end = r
        elif r[0] not in ('FRAME', 'SPE', 'MEM'):
            reasons.append(f'line {k + 1}: unknown kind {r[0]!r}')
    summary = []
    try:
        # MEM
        if [r[2] for r in mem] != ['0', '1', '2']:
            reasons.append(f'MEM rows {[r[2] for r in mem]}, want points 0, 1, 2')
        for r in mem:
            ok = flag(r[ix['items']], 'MEM items')
            qrc = sint(r[ix['digest']], -(1 << 31), (1 << 31) - 1, 'MEM rc')
            if ok:
                tot = uint(r[ix['mem_total']], U32, 'mem_total')
                av = uint(r[ix['mem_available']], U32, 'mem_available')
                if qrc != 0 or tot == 0 or av == 0 or av > tot:
                    reasons.append(f'MEM point {r[2]}: not a valid query result (rc {qrc}, total {tot}, available {av})')
            else:
                if r[ix['mem_total']] != 'UNAVAILABLE' or r[ix['mem_available']] != 'UNAVAILABLE':
                    reasons.append(f'MEM point {r[2]}: a failed query (rc {qrc}) carries numbers, not UNAVAILABLE')
                else:
                    reasons.append(f'MEM point {r[2]} UNAVAILABLE (query rc {qrc}): no memory figure for this arm')
        # FRAME
        idx = [uint(r[1], U32, 'frame') for r in frames]
        if idx != list(range(len(frames))):
            reasons.append('FRAME rows not numbered 0..n-1 in order (a row removed, duplicated or reordered)')
        freqs = {uint(r[ix['tb_freq']], what='tb_freq') for r in frames}
        if len(freqs) != 1 or 0 in freqs:
            reasons.append(f'tb_freq not one nonzero value: {sorted(freqs)}')
        acc = na = 0
        phases = 0
        fseq = {}
        for r in frames:
            ph = uint(r[2], 3, 'phase')
            phases |= 1 << ph
            fseq[r[1]] = (r[3], r[2])
            v = {}
            for name in ELAPSED:
                s = r[ix[name]]
                v[name] = None if s == 'NA' else uint(s, what=name)
            f, fw, nf = v['frame_elapsed_tb'], v['flipwait_elapsed_tb'], v['nonflip_elapsed_tb']
            if f is not None and fw is not None and fw <= f:
                if nf != f - fw:
                    reasons.append(f'frame {r[1]}: nonflip_elapsed_tb {nf} is not frame - flipwait ({f - fw})')
                    break
            elif nf is not None:
                reasons.append(f'frame {r[1]}: nonflip_elapsed_tb {nf} where frame/flipwait are NA or flipwait > frame')
                break
            if nf is not None and f is not None and nf > f:
                reasons.append(f'frame {r[1]}: nonflip_elapsed_tb {nf} > frame_elapsed_tb {f}')
                break
            if any(x is None for x in v.values()):
                na += 1
            else:
                acc += 1
        if acc == 0:
            reasons.append(f'no accepted FRAME row ({na} with NA): a vacuous population')
        summary.append(f'FRAME accepted {acc} NA-excluded {na}')
        # SPE (Plan A r2 2.3): the inventory, then the per-(phase, lane) populations
        fphase = {r[1]: uint(r[2], 3, 'phase') for r in frames}
        seen, sflags, pops = set(), {}, {}
        for r in spe:
            fr = r[1]
            lane = uint(r[4], U32, 'SPE lane')
            if fr not in fseq:
                reasons.append(f'SPE frame {fr}: no FRAME row of that frame (identity)')
                break
            if fphase[fr] not in SPU_PHASES:
                reasons.append(f'SPE frame {fr} lane {lane}: an SPE row on PPU-only phase {fphase[fr]}')
                break
            if r[2] != fseq[fr][1]:
                reasons.append(f'SPE frame {fr} lane {lane}: phase {r[2]}, its FRAME row has phase {fseq[fr][1]} (wrong phase)')
                break
            if r[3] != fseq[fr][0]:
                reasons.append(f'SPE frame {fr} lane {lane}: sequence {r[3]}, its FRAME row has {fseq[fr][0]} (wrong sequence)')
                break
            if lane >= workers:
                reasons.append(f'SPE frame {fr}: lane {lane} is not one of the {workers} expected workers')
                break
            if (fr, r[3], lane) in seen:
                reasons.append(f'SPE frame {fr} sequence {r[3]} lane {lane}: a duplicated SPE row')
                break
            seen.add((fr, r[3], lane))
            uint(r[ix['spu_dec_begin']], U32, 'spu_dec_begin')
            uint(r[ix['spu_dec_end']], U32, 'spu_dec_end')
            fl = spu_window(uint(r[ix['spe_dispatch_tb']], what='spe_dispatch_tb'),
                            uint(r[ix['spe_complete_tb']], what='spe_complete_tb'),
                            flag(r[ix['spu_dec_written']], 'spu_dec_written'))
            key = (fphase[fr], lane)
            pops.setdefault(key, [0, 0])
            if fl:
                sflags[(key, fl)] = sflags.get((key, fl), 0) + 1
                pops[key][1] += 1
            else:
                pops[key][0] += 1
        else:
            for fr, (sq, ph) in fseq.items():
                if fphase[fr] in SPU_PHASES:
                    lanes = {ln for (f2, s2, ln) in seen if f2 == fr and s2 == sq}
                    if lanes != set(range(workers)):
                        reasons.append(f'frame {fr} (SPU phase {ph}): SPE rows for lanes {sorted(lanes)}, want exactly '
                                       f'one per lane 0..{workers - 1}')
                        break
            for ph in sorted({fphase[f] for f in fphase} & SPU_PHASES):
                for lane in range(workers):
                    acc, exc = pops.get((ph, lane), [0, 0])
                    summary.append(f'SPE phase {ph} lane {lane} accepted {acc} excluded {exc}')
                    for (k2, fl2), v2 in sorted(sflags.items()):
                        if k2 == (ph, lane):
                            summary.append(f'SPE phase {ph} lane {lane} {fl2} {v2}')
                    if acc == 0:
                        why = ' '.join(f'{k[1]} {v}' for k, v in sorted(sflags.items()) if k[0] == (ph, lane))
                        reasons.append(f'SPE ticks UNAVAILABLE for phase {ph} lane {lane}: no accepted row ({exc} '
                                       f'excluded: {why or "none present"}); this run cannot support that measurement')
        summary.append(f'SPE rows {len(spe)} for {workers} expected workers')
        # END
        if end is None:
            reasons.append('END row missing or not last')
        else:
            n = uint(end[1], U32, 'END frames')
            mask = uint(end[2], 15, 'END phase mask')
            checked = uint(end[5], U32, 'END checked')
            erc = sint(end[6], -(1 << 31), (1 << 31) - 1, 'END rc')
            if n != len(frames):
                reasons.append(f'END frames={n} for {len(frames)} FRAME rows (a row removed or duplicated)')
            if checked != n:
                reasons.append(f'END checked={checked} for frames={n}')
            if erc != 0:
                reasons.append(f'END rc={erc}: the run failed')
            if mask != phases:
                reasons.append(f'END phase mask {mask} differs from the FRAME phases {phases}')
            if profile == 'production' and mask != 15:
                reasons.append(f'phase mask {mask}: not all four phases ran')
    except Bad as e:
        reasons.append(str(e))
    return reasons, summary


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('what', choices=['bench', 'sample', 'gpu'])
    p.add_argument('csv')
    p.add_argument('--receipt')
    p.add_argument('--profile', choices=['production', 'test'], default='production')
    p.add_argument('--cases')
    p.add_argument('--r', type=int)
    p.add_argument('--arm', choices=sorted(ARMS))
    p.add_argument('--workers', type=int)
    p.add_argument('--manifest-self')
    p.add_argument('--client-self')
    a = p.parse_args(argv)
    reasons, summary = [], []
    if a.profile == 'production':
        if a.cases or a.r is not None:
            reasons.append('--cases/--r are only for --profile test (production pins the inventory and R)')
        cases, r = CASES_PRODUCTION, R
    else:
        if a.what == 'bench' and not a.cases:
            reasons.append('--profile test needs --cases')
        if a.what != 'sample' and a.r is None:
            reasons.append('--profile test needs --r')
        cases = [c for c in (a.cases or '').split(',') if c]
        if any(c not in CASES for c in cases) or len(set(cases)) != len(cases):
            reasons.append(f'--cases {a.cases!r}: unknown or repeated case')
        r = a.r if a.r is not None else R
        if r < 1:
            reasons.append('--r must be at least 1')
    workers = None
    if a.what == 'sample':
        if a.profile == 'production':
            if a.workers is not None or not a.arm:
                reasons.append('sample, production profile: --arm is required (it binds the expected workers); '
                               '--workers is only for --profile test')
            else:
                workers = ARMS[a.arm]
        else:
            if a.arm or a.workers is None or a.workers < 1:
                reasons.append('sample, test profile: --workers N (>= 1) is required, --arm is not allowed')
            else:
                workers = a.workers
    if not reasons:
        try:
            if a.what == 'bench':
                reasons, summary = verify_bench(a.csv, a.receipt, a.profile, cases, r)
            elif a.what == 'gpu':
                reasons, summary = verify_gpu(a.csv, a.profile, r)
            else:
                reasons, summary = verify_sample(a.csv, a.profile, workers)
        except (OSError, ValueError, IndexError, UnicodeDecodeError, Bad) as e:
            reasons = [f'unreadable: {e}']
    if (a.manifest_self or a.client_self) and a.manifest_self != a.client_self:
        reasons.append(f'.self hash mismatch: manifest {a.manifest_self} client {a.client_self}')
    for s in summary:
        print(f'ob_verify: population {s}')
    for x in reasons:
        print(f'ob_verify: {x}')
    print('ob_verify: ' + ('FAIL' if reasons else 'PASS'))
    return 1 if reasons else 0


if __name__ == '__main__':
    sys.exit(main())
