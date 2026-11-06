#!/usr/bin/env python3
"""Controls of tools/ob_verify.py (Plan A r2 2.3 and 3.3).  Fixtures are generated here (the production inventory
and R unless a control says --profile test).  Unmodified fixtures and the production-path CSVs of the host models
PASS; every control FAILs with its own message; the 2.3 window controls flag exactly the rows they change; exit
status checked as exactly 0 or 1 every time.
usage: test_ob_verify.py --out NEW_DIR [--host-csv CSV] [--life-csv CSV --life-gpu-csv CSV] [--gpu-rt CSV ...]"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VERIFY = HERE.parent / 'tools' / 'ob_verify.py'
sys.path.insert(0, str(VERIFY.parent))
import ob_verify as V  # noqa: E402

NL = chr(10)
checks = failures = 0


def check(ok, name):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f'FAIL {name}')


def run(args):
    r = subprocess.run([sys.executable, '-B', '-I', str(VERIFY)] + args, capture_output=True, text=True)
    if r.returncode not in (0, 1):
        check(False, f'exit status {r.returncode} is neither 0 nor 1 for {args}')
    return r.returncode, r.stdout


SEED = 4242


def bench_rows(cases=V.CASES_PRODUCTION, r=V.R, seed=SEED):
    sched = V.schedule(seed, [(V.CASES.index(c), c == 'P-cold') for c in cases], r)
    head = [f'# OB seed={seed} blocks={len(sched)} digest={V.digest(sched):016x} tb_freq=79800000', ','.join(V.HEADER)]
    kinds = {1: 'warm', 2: 'timed', 3: 'cold'}
    rows, cmd_seq = [], 0
    for i, (c, t, k, rep) in enumerate(sched):
        name = V.CASES[c]
        led = dict.fromkeys(V.FIELDS, 0)
        if t == 0:
            led = V.planned(name)
            if k == 1:
                led['regs'] = V.WARM_REGS.get(name, 0)
        f = {'kind': 'BLK', 'index': str(i), 'case': name, 'tu': V.TUS[t], 'block': kinds[k], 'rep': str(rep)}
        f.update({x: str(led[x]) for x in V.FIELDS})
        f['rc'] = '0'
        if name.startswith('S-'):
            cmd_seq = cmd_seq % 65535 + 1
            cmd = (cmd_seq << 16) | (c << 8) | (0 if t == 0 else 1)
            f.update(t0='5000', t1='4000', counted='1', ppu_t0=str(10**6 + 1000 * i), ppu_t1=str(10**6 + 1000 * i + 700),
                     dec_reset='1', spu_cmd=str(cmd), spu_echo=str(cmd))
        else:
            f.update(t0='1000', t1='1100', counted='0', ppu_t0='0', ppu_t1='0', dec_reset='0', spu_cmd='0',
                     spu_echo='0')
        rows.append(f)
    return head, rows


def bench_lines(head, rows, end=None):
    out = list(head) + [','.join(f[h] for h in V.HEADER) for f in rows]
    out.append(f'END,{len(rows) if end is None else end}')
    return out


def gpu_lines(seed=99, r=V.R):
    sched = V.gpu_schedule(seed, r)
    out = [f'# OBG seed={seed} blocks={len(sched)} digest={V.digest(sched):016x}', ','.join(V.GPU_HEADER)]
    size = {0: 2000, 1: 2000 + 64 * 48, 2: 2000 + 64 * 48, 3: 0, 4: 0}
    for i, (c, _, k, rep) in enumerate(sched):
        kind = 'warm' if k == 1 else 'timed'
        out.append(f'GPU,{i},{V.G_CASES[c]},{kind},{rep},10,20,{size[c]},100,200,0')
    out.append(f'END,{len(sched)}')
    return out


def sample_lines(phases=(0,), frames=12, lanes=2):
    z = ['0'] * len(V.SAMPLE_HEADER)
    ix = {h: i for i, h in enumerate(V.SAMPLE_HEADER)}
    out = [','.join(V.SAMPLE_HEADER)]

    def mem(p):
        r = list(z)
        r[0], r[2], r[ix['items']], r[ix['digest']] = 'MEM', str(p), '1', '0'
        r[ix['mem_total']], r[ix['mem_available']] = '268435456', str(200000000 - p)
        return ','.join(r)
    out += [mem(0), mem(1)]
    mask = 0
    for f in range(frames):
        ph = phases[f * len(phases) // frames]
        mask |= 1 << ph
        if ph in (1, 2):
            for lane in range(lanes):
                s = list(z)
                s[0], s[1], s[2], s[3], s[4] = 'SPE', str(f), str(ph), str(f + 1), str(lane)
                s[ix['spu_dec_begin']], s[ix['spu_dec_end']], s[ix['spu_dec_written']] = '4000000000', '3999000000', '1'
                s[ix['spe_dispatch_tb']], s[ix['spe_complete_tb']] = str(10**7 * (f + 1)), str(10**7 * (f + 1) + 5000)
                out.append(','.join(s))
        r = list(z)
        r[0], r[1], r[2], r[3] = 'FRAME', str(f), str(ph), str(f + 1)
        for k, v in (('compute_elapsed_tb', '500'), ('frame_elapsed_tb', '2000'), ('submit_elapsed_tb', '300'),
                     ('flipwait_elapsed_tb', '1200'), ('nonflip_elapsed_tb', '800'), ('drain_elapsed_tb', '40'),
                     ('profcall_elapsed_tb', '9'), ('tb_freq', '79800000')):
            r[ix[k]] = v
        out.append(','.join(r))
    out.append(mem(2))
    e = list(z)
    e[0], e[1], e[2], e[5], e[6] = 'END', str(frames), str(mask), str(frames), '0'
    out.append(','.join(e))
    return out


def write(d, name, lines):
    p = d / name
    p.write_text(NL.join(lines) + NL, encoding='ascii', newline=NL)
    return str(p)


def expect_fail(args, text, name):
    rc, out = run(args)
    check(rc == 1 and text in out, f'{name} FAILS ({out.strip()[-160:]!r})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--host-csv')
    ap.add_argument('--life-csv')
    ap.add_argument('--life-gpu-csv')
    ap.add_argument('--gpu-rt', action='append', default=[])
    ap.add_argument('--gpu-rt-neg', action='append', default=[])
    a = ap.parse_args()
    a.out.mkdir()
    d = a.out
    head, rows = bench_rows()
    base = bench_lines(head, rows)
    bp = write(d, 'bench.csv', base)
    rc, out = run(['bench', bp])
    check(rc == 0 and 'PASS' in out, f'unmodified production bench fixture PASS ({out.strip()[-200:]})')
    n = len(rows)
    rec = write(d, 'bench-end.txt', [f'BENCH_END rows={n} invalid=0 ledger_mismatches=0 receipt=complete'])
    rc, out = run(['bench', bp, '--receipt', rec])
    check(rc == 0, f'bench fixture with its exact receipt PASS ({out.strip()[-120:]})')
    # the production-path CSVs of the host models
    if a.host_csv:
        rc, out = run(['bench', a.host_csv, '--profile', 'test', '--cases',
                       'P-hot,P-nest8,P-cold,P-drop,P-frame,P-empty', '--r', '200'])
        check(rc == 0 and 'PASS' in out, f'host ledger model CSV (real runtime core) PASS ({out.strip()[-200:]})')
        rc, out = run(['bench', a.host_csv])
        check(rc == 1 and 'pinned' in out, 'the host model subset under the production profile FAILS (pinned inventory)')
    if a.life_csv:
        rc, out = run(['bench', a.life_csv, '--profile', 'test', '--cases', ','.join(V.CASES_PRODUCTION), '--r', '1'])
        check(rc == 0 and 'PASS' in out, f'the driver baseline CSV (test_ob_life, production orchestration) PASS '
                                          f'({out.strip()[-200:]})')
    if a.life_gpu_csv:
        rc, out = run(['gpu', a.life_gpu_csv, '--profile', 'test', '--r', '1'])
        check(rc == 0 and 'PASS' in out, f'the driver baseline GPU CSV PASS ({out.strip()[-200:]})')
    for g in a.gpu_rt:
        rc, out = run(['gpu', g, '--profile', 'test', '--r', '3'])
        check(rc == 0 and 'PASS' in out, f'the real-runtime GPU CSV {Path(g).name} PASS ({out.strip()[-200:]})')
    for g in a.gpu_rt_neg:
        expect_fail(['gpu', g, '--profile', 'test', '--r', '3'], 'g1 - g0',
                    f'the exhausted real-runtime GPU CSV {Path(g).name} (endpoints not emitted)')
    # --- the reviewer's false passes, regenerated in this schema ---
    ep = d / 'empty-receipt.txt'
    ep.write_bytes(b'')
    expect_fail(['bench', bp, '--receipt', str(ep)], '(empty)', 'an empty --receipt file')
    expect_fail(['bench', bp, '--receipt', str(d / 'absent.txt')], 'receipt:', 'an absent --receipt file')
    x = [dict(f) for f in rows]
    k = next(i for i, f in enumerate(x) if f['case'] == 'S-pair')
    x[k]['rc'] = '2147418113'
    expect_fail(['bench', write(d, 'spu-rc.csv', bench_lines(head, x))], 'rc 2147418113', 'a nonzero S-pair rc')
    x = [dict(f) for f in rows]
    x[k]['counted'], x[k]['sseq'] = '0', '999999'
    rc, out = run(['bench', write(d, 'uncounted.csv', bench_lines(head, x))])
    check(rc == 1 and 'must be counted' in out and 'ledger' in out,
          'counted=0 hiding a corrupted SPU ledger FAILS (both the counted status and the ledger)')
    z = [f'# OB seed={SEED} blocks=0 digest={V.digest([]):016x} tb_freq=79800000', ','.join(V.HEADER), 'END,0']
    zr = write(d, 'zero.receipt', ['BENCH_END rows=0 invalid=0 ledger_mismatches=0 receipt=complete'])
    expect_fail(['bench', write(d, 'zero.csv', z), '--receipt', zr], 'pinned schedule', 'a zero-block bench with a '
                'matching empty digest and receipt')
    s = sample_lines()
    sp = write(d, 'sample.csv', s)
    rc, out = run(['sample', sp, '--profile', 'test', '--workers', '2'])
    check(rc == 0 and 'PASS' in out, f'unmodified sample fixture (test profile) PASS ({out.strip()[-200:]})')
    y = list(s)
    e = y[-1].split(',')
    e[6] = '1'
    y[-1] = ','.join(e)
    expect_fail(['sample', write(d, 'sample-rc1.csv', y), '--profile', 'test', '--workers', '2'], 'END rc=1', 'sample END rc=1')
    y = list(s)
    m = y[1].split(',')
    m[5], m[6] = '0', '-2147418100'
    y[1] = ','.join(m)
    expect_fail(['sample', write(d, 'sample-mem-failed.csv', y), '--profile', 'test', '--workers', '2'], 'carries numbers',
                'a failed MEM query carrying numbers')
    # --- bench controls ---
    expect_fail(['bench', write(d, 'removed.csv', base[:3] + base[4:])], 'index', 'one row removed')
    x = [dict(f) for f in rows]
    x[10]['case'] = 'P-frame' if x[10]['case'] != 'P-frame' else 'P-hot'
    expect_fail(['bench', write(d, 'missing-block.csv', bench_lines(head, x))], 'not the seeded block',
                'one block missing (replaced)')
    expect_fail(['bench', write(d, 'dup.csv', base[:12] + [base[11]] + base[12:])], 'index', 'a duplicated row')
    x = [dict(f) for f in rows]
    j = next(i for i in range(40, len(x) - 1) if (x[i]['case'], x[i]['tu']) != (x[i + 1]['case'], x[i + 1]['tu']))
    x[j], x[j + 1] = x[j + 1], x[j]
    x[j]['index'], x[j + 1]['index'] = str(j), str(j + 1)
    expect_fail(['bench', write(d, 'order.csv', bench_lines(head, x))], 'not the seeded block', 'a block out of order')
    y = [base[0], ','.join(V.HEADER[:-1])] + [','.join(r.split(',')[:-1]) if r.startswith('BLK') else r for r in base[2:]]
    expect_fail(['bench', write(d, 'column.csv', y)], 'column header', 'a missing column')
    x = [dict(f) for f in rows]
    q = next(i for i, f in enumerate(x) if f['case'] == 'P-drop' and f['tu'] == 'on' and f['block'] == 'timed')
    x[q]['dropped'] = str(int(x[q]['dropped']) - 1)
    expect_fail(['bench', write(d, 'ledger.csv', bench_lines(head, x))], 'dropped', 'a ledger count one short')
    expect_fail(['bench', bp, '--manifest-self', 'aa', '--client-self', 'ab'], '.self hash mismatch', 'a .self mismatch')
    rec2 = write(d, 'bench-end-bad.txt', [f'BENCH_END rows={n - 1} invalid=0 ledger_mismatches=0 receipt=complete'])
    expect_fail(['bench', bp, '--receipt', rec2], 'receipt does not match', 'a receipt that disagrees with the CSV')
    for bad in ('2', 'x', '', '-1'):
        x = [dict(f) for f in rows]
        x[5]['counted'] = bad
        expect_fail(['bench', write(d, f'counted-{len(bad)}{bad.strip("-")}.csv', bench_lines(head, x))], 'counted',
                    f'a malformed counted value {bad!r}')
    for bi, bad in enumerate(('-1', '1e3', str(1 << 64), ' 5')):
        x = [dict(f) for f in rows]
        x[7]['t0'] = bad
        expect_fail(['bench', write(d, f't0-{bi}.csv', bench_lines(head, x))], 't0',
                    f'a malformed t0 {bad!r}')
    x = [dict(f) for f in rows]
    q = next(i for i, f in enumerate(x) if f['case'].startswith('S-'))
    x[q]['t0'] = str(1 << 32)
    expect_fail(['bench', write(d, 'dec-range.csv', bench_lines(head, x))], 'out of range',
                'a decrementer reading beyond 32 bits')
    x = [dict(f) for f in rows]
    for f in x:
        if f['case'] == 'P-hot' and f['tu'] == 'on' and f['block'] == 'timed':
            f['t0'], f['t1'] = '2000', '1000'
    expect_fail(['bench', write(d, 'all-invalid.csv', bench_lines(head, x))], 'P-hot/on: no accepted',
                'an all-INVALID PPU population')
    rcp = write(d, 'all-invalid.receipt', [f'BENCH_END rows={n} invalid={V.R} ledger_mismatches=0 receipt=complete'])
    rc, out = run(['bench', str(d / 'all-invalid.csv'), '--receipt', rcp])
    check(rc == 1 and 'vacuous' in out and 'receipt does not match' not in out,
          'the all-INVALID population still FAILS with a receipt that counts it')
    x = [dict(f) for f in rows]
    for f in x:
        if f['case'] == 'S-pair' and f['tu'] == 'on' and f['block'] == 'timed':
            f['dec_reset'] = '0'
    expect_fail(['bench', write(d, 'all-noreset.csv', bench_lines(head, x))], 'S-pair/on: no accepted',
                'an all-MISSING-RESET SPU population')
    x = [dict(f) for f in rows]
    x[k]['spu_echo'] = str(int(x[k]['spu_cmd']) ^ 0x10000)
    expect_fail(['bench', write(d, 'echo.csv', bench_lines(head, x))], 'identity', 'a reply echo that is not the command')
    x = [dict(f) for f in rows]
    x[k + 1]['spu_cmd'] = x[k + 1]['spu_echo'] = str(int(x[k]['spu_cmd']))
    expect_fail(['bench', write(d, 'cmdseq.csv', bench_lines(head, x))], 'identity', 'a repeated command sequence')
    x = [dict(f) for f in rows]
    p0 = next(i for i, f in enumerate(x) if f['case'] == 'P-hot')
    x[p0]['ppu_t0'] = '5'
    expect_fail(['bench', write(d, 'ppu-spu-fields.csv', bench_lines(head, x))], 'carries SPU fields',
                'a PPU row carrying SPU fields')
    expect_fail(['bench', bp, '--r', '3'], 'only for --profile test', 'test parameters under the production profile')
    expect_fail(['bench', bp, '--profile', 'test', '--r', '200'], 'needs --cases', 'the test profile without --cases')
    expect_fail(['bench', write(d, 'early-end.csv', bench_lines(head, rows[:-5], end=len(rows)))], 'BLK rows',
                'an early stop whose END claims the planned count')
    # --- Plan A r2 2.3 window controls (one row each; the population stays non-vacuous) ---
    sp_on = [i for i, f in enumerate(rows) if f['case'] == 'S-pair' and f['tu'] == 'on' and f['block'] == 'timed']

    def window(name, mut, want_flag):
        x = [dict(f) for f in rows]
        mut(x[sp_on[0]])
        rc, out = run(['bench', write(d, f'w-{name}.csv', bench_lines(head, x))])
        if want_flag:
            check(rc == 0 and f'S-pair/on {want_flag} 1' in out and f'S-pair/on accepted {V.R - 1} excluded 1' in out,
                  f'2.3 {name}: exactly that row flagged {want_flag} and excluded ({out.strip()[-160:]!r})')
        else:
            check(rc == 0 and f'S-pair/on accepted {V.R} excluded 0' in out,
                  f'2.3 {name}: the row is accepted ({out.strip()[-160:]!r})')

    def setp(el):
        def f(r):
            r['ppu_t1'] = str(int(r['ppu_t0']) + el)
        return f
    window('exact-bound', setp(1 << 31), 'OVERRUN-POSSIBLE')
    window('one-under', setp((1 << 31) - 1), None)
    window('missing-reset', lambda r: r.update(dec_reset='0'), 'MISSING-RESET')
    window('delayed-reply', setp(3 * 10**9), 'OVERRUN-POSSIBLE')
    window('regressed-clock', lambda r: r.update(ppu_t1=str(int(r['ppu_t0']) - 1)), 'REGRESSED')
    window('wrap-possible', setp(1 << 33), 'OVERRUN-POSSIBLE')
    window('decrementer-wraps-short-interval', lambda r: r.update(t0='100', t1=str((1 << 32) - 100)), None)
    # --- gpu controls ---
    g = gpu_lines()
    gp = write(d, 'gpu.csv', g)
    rc, out = run(['gpu', gp])
    check(rc == 0 and 'PASS' in out, f'unmodified production gpu fixture PASS ({out.strip()[-160:]})')
    expect_fail(['gpu', write(d, 'gpu-removed.csv', g[:5] + g[6:])], 'GPU rows', 'a removed GPU row')

    def first(lines, case):
        return next(i for i in range(2, len(lines)) if lines[i].split(',')[2] == case and lines[i].split(',')[3] == 'timed')
    y = list(g)
    i1 = first(y, 'G-g1')
    f = y[i1].split(',')
    f[7] = str(int(f[7]) - 4)
    y[i1] = ','.join(f)
    expect_fail(['gpu', write(d, 'gpu-bytes.csv', y)], 'one value per variant', 'a g1 row 4 bytes short')
    f[10] = '2'
    y[i1] = ','.join(f)
    rc, out = run(['gpu', write(d, 'gpu-wrap.csv', y)])
    check(rc == 0, 'the same short row flagged WRAP (rc 2) is excluded, not an error')
    y = [ln if ',G-g2,' not in ln else ','.join(ln.split(',')[:7] + [str(int(ln.split(',')[7]) - 48)] + ln.split(',')[8:])
         for ln in g]
    expect_fail(['gpu', write(d, 'gpu-g2.csv', y)], 'g2 must match g1', 'every g2 row one endpoint pair short')
    y = list(g)
    j = next(i for i in range(20, len(y) - 2) if y[i].split(',')[2:5] != y[i + 1].split(',')[2:5])
    p1, p2 = y[j].split(','), y[j + 1].split(',')
    p1[1], p2[1] = p2[1], p1[1]
    y[j], y[j + 1] = ','.join(p2), ','.join(p1)
    expect_fail(['gpu', write(d, 'gpu-order.csv', y)], 'not the seeded block', 'a GPU block out of order')
    expect_fail(['gpu', write(d, 'gpu-seed.csv', [g[0].replace('seed=99', 'seed=98')] + g[1:])], 'digest',
                'a GPU header seed that is not the schedule')
    expect_fail(['gpu', write(d, 'gpu-early.csv', g[:12] + ['END,10'])], 'early stop', 'an early GPU stop (END = rows)')
    for v, text in (('1', 'rc 1'), ('4', 'rc 4'), ('8', 'rc 8'), ('16', 'rc')):
        y = list(g)
        f = y[i1].split(',')
        f[10] = v
        y[i1] = ','.join(f)
        expect_fail(['gpu', write(d, f'gpu-rc{v}.csv', y)], text, f'a GPU row with rc {v}')
    y = [','.join(ln.split(',')[:10] + ['2']) if ',G-g0,timed,' in ln else ln for ln in g]
    expect_fail(['gpu', write(d, 'gpu-allwrap.csv', y)], 'G-g0: no accepted', 'an all-WRAP G-g0 population')
    y = list(g)
    f = y[i1].split(',')
    f[8] = 'x'
    y[i1] = ','.join(f)
    expect_fail(['gpu', write(d, 'gpu-malformed.csv', y)], 't0', 'a malformed GPU field')
    # --- sample controls ---
    ps = sample_lines(phases=(0, 1, 2, 3), frames=16, lanes=6)
    rc, out = run(['sample', write(d, 'sample-prod.csv', ps), '--arm', 'P1'])
    check(rc == 0 and 'PASS' in out and 'SPE rows 48 for 6 expected workers' in out, f'the production sample fixture (4 phases) PASS '
                                                                  f'({out.strip()[-200:]})')
    expect_fail(['sample', sp, '--arm', 'P1'], 'not all four phases', 'a one-phase sample under the production profile')
    ix = {h: i for i, h in enumerate(V.SAMPLE_HEADER)}

    def frame_mut(lines, fn, row=0):
        y = list(lines)
        k2 = [i for i, ln in enumerate(y) if ln.startswith('FRAME,')][row]
        r = y[k2].split(',')
        fn(r)
        y[k2] = ','.join(r)
        return y
    y = frame_mut(s, lambda r: r.__setitem__(ix['nonflip_elapsed_tb'], '2500'))
    expect_fail(['sample', write(d, 'nonflip.csv', y), '--profile', 'test', '--workers', '2'], 'not frame - flipwait', 'nonflip != frame - flipwait')
    expect_fail(['sample', write(d, 'sample-column.csv', [','.join(r.split(',')[:-1]) for r in s]), '--profile', 'test', '--workers', '2'],
                'header differs', 'a missing sample column')
    fr = [i for i, ln in enumerate(s) if ln.startswith('FRAME,')]
    expect_fail(['sample', write(d, 'sample-removed.csv', s[:fr[3]] + s[fr[3] + 1:]), '--profile', 'test', '--workers', '2'], 'FRAME rows',
                'a removed FRAME row')
    expect_fail(['sample', write(d, 'sample-mem.csv', s[:1] + s[2:]), '--profile', 'test', '--workers', '2'], 'MEM rows', 'a missing MEM point')
    y = list(s)
    m = y[2].split(',')
    m[5], m[6], m[ix['mem_total']], m[ix['mem_available']] = '0', '-5', 'UNAVAILABLE', 'UNAVAILABLE'
    y[2] = ','.join(m)
    expect_fail(['sample', write(d, 'sample-mem-unavail.csv', y), '--profile', 'test', '--workers', '2'], 'UNAVAILABLE (query rc -5)',
                'an UNAVAILABLE MEM point (reported, never a zero figure)')
    y = list(s)
    m = y[1].split(',')
    m[ix['mem_available']] = '0'
    y[1] = ','.join(m)
    expect_fail(['sample', write(d, 'sample-mem-zero.csv', y), '--profile', 'test', '--workers', '2'], 'not a valid query result',
                'a zero memory figure')
    y = list(s)
    e = y[-1].split(',')
    e[5] = str(int(e[5]) - 1)
    y[-1] = ','.join(e)
    expect_fail(['sample', write(d, 'sample-checked.csv', y), '--profile', 'test', '--workers', '2'], 'END checked', 'END checked != frames')
    y = list(s)
    e = y[-1].split(',')
    e[2] = '3'
    y[-1] = ','.join(e)
    expect_fail(['sample', write(d, 'sample-mask.csv', y), '--profile', 'test', '--workers', '2'], 'phase mask', 'a wrong END phase mask')
    y = frame_mut(s, lambda r: (r.__setitem__(ix['frame_elapsed_tb'], 'NA'), r.__setitem__(ix['nonflip_elapsed_tb'], 'NA')))
    rc, out = run(['sample', write(d, 'sample-na.csv', y), '--profile', 'test', '--workers', '2'])
    check(rc == 0 and 'NA-excluded 1' in out, f'an NA frame row is excluded and counted ({out.strip()[-120:]!r})')
    y = list(s)
    for i in fr:
        r = y[i].split(',')
        r[ix['frame_elapsed_tb']], r[ix['nonflip_elapsed_tb']] = 'NA', 'NA'
        y[i] = ','.join(r)
    expect_fail(['sample', write(d, 'sample-allna.csv', y), '--profile', 'test', '--workers', '2'], 'no accepted FRAME', 'an all-NA FRAME population')
    y = frame_mut(s, lambda r: r.__setitem__(ix['flipwait_elapsed_tb'], '2500'))
    expect_fail(['sample', write(d, 'sample-fw-gt.csv', y), '--profile', 'test', '--workers', '2'], 'flipwait > frame',
                'a numeric nonflip where flipwait > frame (an unsigned underflow)')
    y = frame_mut(s, lambda r: r.__setitem__(ix['frame_elapsed_tb'], str((1 << 64) - 5)))
    expect_fail(['sample', write(d, 'sample-huge.csv', y), '--profile', 'test', '--workers', '2'], 'nonflip',
                'a wrapped (huge) frame elapsed exported as a number')
    spe = [i for i, ln in enumerate(ps) if ln.startswith('SPE,')]
    y = list(ps)
    r = y[spe[0]].split(',')
    r[ix['spe_complete_tb']] = str(int(r[ix['spe_dispatch_tb']]) - 1)
    y[spe[0]] = ','.join(r)
    rc, out = run(['sample', write(d, 'sample-spe-regressed.csv', y), '--arm', 'P1'])
    check(rc == 0 and 'SPE phase 1 lane 0 REGRESSED 1' in out and 'SPE phase 1 lane 0 accepted 3 excluded 1' in out, 'an SPE row with a regressed PPU interval is flagged')
    y = list(ps)
    r = y[spe[1]].split(',')
    r[ix['spu_dec_written']] = '0'
    y[spe[1]] = ','.join(r)
    rc, out = run(['sample', write(d, 'sample-spe-noreset.csv', y), '--arm', 'P1'])
    check(rc == 0 and 'SPE phase 1 lane 1 MISSING-RESET 1' in out, 'an SPE row without a reset is flagged MISSING-RESET')
    y = list(ps)
    r = y[spe[2]].split(',')
    r[ix['spe_complete_tb']] = str(int(r[ix['spe_dispatch_tb']]) + (1 << 31))
    y[spe[2]] = ','.join(r)
    rc, out = run(['sample', write(d, 'sample-spe-bound.csv', y), '--arm', 'P1'])
    check(rc == 0 and 'SPE phase 1 lane 2 OVERRUN-POSSIBLE 1' in out, 'an SPE row at the exact RMAX bound is flagged OVERRUN-POSSIBLE')
    y = list(ps)
    r = y[spe[0]].split(',')
    r[3] = '999'
    y[spe[0]] = ','.join(r)
    expect_fail(['sample', write(d, 'sample-spe-identity.csv', y), '--arm', 'P1'], 'wrong sequence',
                'an SPE row whose sequence is not that of its FRAME row')
    # --- the SPE inventory (the reviewer's four repros, regenerated, and the per-lane controls) ---
    def spe_mut(fn, name):
        y = []
        for ln in ps:
            if ln.startswith('SPE,'):
                r = ln.split(',')
                out = fn(r)
                if out is None:
                    continue
                y.extend(out)
            else:
                y.append(ln)
        return write(d, name, y)
    P1 = ['--arm', 'P1']
    expect_fail(['sample', spe_mut(lambda r: None, 'spe-none.csv')] + P1, 'want exactly one per lane',
                'every SPE row removed')
    def noreset(r):
        r[ix['spu_dec_written']] = '0'
        return [','.join(r)]
    expect_fail(['sample', spe_mut(noreset, 'spe-noreset-all.csv')] + P1, 'SPE ticks UNAVAILABLE',
                'every SPE reset missing')
    first = [True]
    def dup(r):
        if first[0]:
            first[0] = False
            return [','.join(r), ','.join(r)]
        return [','.join(r)]
    expect_fail(['sample', spe_mut(dup, 'spe-dup.csv')] + P1, 'duplicated SPE row', 'a duplicated SPE row')
    def lane999(r):
        r[4] = '999'
        return [','.join(r)]
    expect_fail(['sample', spe_mut(lane999, 'spe-lane999.csv')] + P1, 'not one of the 6 expected workers',
                'every lane 999')
    def drop_one(r):
        return None if (r[1], r[4]) == ('5', '3') else [','.join(r)]
    expect_fail(['sample', spe_mut(drop_one, 'spe-one-lane-lost.csv')] + P1, 'frame 5 (SPU phase 1): SPE rows for lanes',
                'one lane of one frame lost')
    def lane3_noreset(r):
        if r[4] == '3' and r[2] == '2':
            r[ix['spu_dec_written']] = '0'
        return [','.join(r)]
    expect_fail(['sample', spe_mut(lane3_noreset, 'spe-lane-excluded.csv')] + P1,
                'SPE ticks UNAVAILABLE for phase 2 lane 3', 'one lane of one phase all excluded')
    def wrong_phase(r):
        if (r[1], r[4]) == ('6', '0'):
            r[2] = '2'
        return [','.join(r)]
    expect_fail(['sample', spe_mut(wrong_phase, 'spe-wrong-phase.csv')] + P1, 'wrong phase', 'an SPE row with the wrong phase')
    def wrong_seq(r):
        if (r[1], r[4]) == ('9', '4'):
            r[3] = str(int(r[3]) + 1)
        return [','.join(r)]
    expect_fail(['sample', spe_mut(wrong_seq, 'spe-wrong-seq.csv')] + P1, 'wrong sequence', 'an SPE row with the wrong sequence')
    y = list(ps)
    k0 = next(i for i, ln in enumerate(y) if ln.startswith('FRAME,1,'))
    y.insert(k0, y[next(i for i, ln in enumerate(y) if ln.startswith('SPE,'))].replace('SPE,4,1,5,', 'SPE,1,0,2,', 1))
    expect_fail(['sample', write(d, 'spe-ppu-phase.csv', y)] + P1, 'PPU-only phase', 'an SPE row on a PPU-only phase')
    expect_fail(['sample', write(d, 'sample-prod2.csv', ps)], '--arm is required', 'production without --arm')
    expect_fail(['sample', str(d / 'sample-prod.csv'), '--arm', 'P1', '--workers', '6'], '--arm is required',
                '--workers under the production profile')
    expect_fail(['sample', str(d / 'sample-prod.csv'), '--profile', 'test'], '--workers N', 'the test profile without --workers')
    expect_fail(['sample', str(d / 'sample-prod.csv'), '--profile', 'test', '--workers', '5'], 'not one of the 5',
                'a run with more workers than the bound profile')
    expect_fail(['sample', sp, '--profile', 'test', '--workers', '2', '--manifest-self', 'aa', '--client-self', 'ab'], '.self hash mismatch',
                'a sample .self mismatch')
    print(f'test_ob_verify: {checks} checks, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
