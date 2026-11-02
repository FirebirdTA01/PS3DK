#!/usr/bin/env python3
"""P3.3 R1: the RSX (GPU) domain core as a HOST MODEL (design r2 + r3/r4/r5 deltas, codex GO 10:27; corrections codex
11:52).  Not target evidence: a GPU emulator executes the submitted pairs; A1 (report visible before its label) and
A2 (coherent report read) are assumptions; the mapping is UNMEASURED.

Rows (exit 0 only if all pass):
  1. the deterministic traces (rsx_model.c), plain and ASan/UBSan: every in-trace claim;
  2. the fail-closed checker (rsx_oracle.py, pinned inventory) on every capture against the producer/GPU truth,
     including the intentionally UNDETECTED A1/A2 counterexamples (the checker sees the wrong value; the runtime
     cannot);
  3. the committed reader (`ps3dk-prof check`) on every capture, exact exit codes;
  4. threaded runs under TSan (run, seal-live, detach): report-free; the NEGATIVE control (no snap lock, its own
     directory) must be reported; the threaded captures through the checker and the reader;
  5. core mutants, each the RED of one fix: killed by its NAMED trace (an in-trace claim or the checker);
  6. oracle damage controls: each damaged copy of the plain captures fails with its named reason and nothing else;
  7. no allocation: the core object references no heap function.

usage: r1-test.py OUT --reader READER"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('out', type=Path)
ap.add_argument('--reader', type=Path, required=True)
a = ap.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
src = root / 'sdk/libps3dkprof/src/rsx'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
base = ['-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror', f'-I{src}', f'-I{here}']
rows = []


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def run(cmd, log, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + r.stderr)
    return r


def oracle(d, suite, log):
    return run([sys.executable, '-I', here / 'rsx_oracle.py', d, '--suite', suite], log)


def reader_check(d):
    bad, n = [], 0
    for e in sorted(d.glob('*.expect')):
        lines = e.read_text().splitlines()
        if any(l.startswith('noreader') for l in lines):
            continue
        want = [l.split()[1] for l in lines if l.startswith('exit ')]
        r = subprocess.run([str(a.reader), 'check', str(e.with_suffix('.prof'))], capture_output=True, text=True)
        n += 1
        if len(want) != 1 or str(r.returncode) != want[0]:
            bad.append(f'{e.stem}: rc {r.returncode} want {want}')
    return bad, n


# 1-3: deterministic traces, plain and asan
plain = None
for v, extra in (('plain', []), ('asan', ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'])):
    d = out / f'traces-{v}'
    d.mkdir()
    r = run([cc, *base, *extra, src / 'prg.c', here / 'rsx_model.c', '-o', d / 'r1m'], d / 'build.log')
    if r.returncode != 0:
        row(f'traces ({v}) build', False)
        continue
    r = run([d / 'r1m', d], d / 'run.log', env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))
    m = re.search(r'R1 traces (\d+), failures (\d+)', r.stdout)
    clean = 'runtime error' not in r.stderr and 'AddressSanitizer' not in r.stderr
    row(f'deterministic traces ({v})', r.returncode == 0 and m is not None and m.group(2) == '0' and clean,
        f'({m.group(1) if m else "?"} traces)')
    o = oracle(d, 'traces', d / 'oracle.log')
    row(f'oracle on the {v} captures', o.returncode == 0, (o.stdout.strip().splitlines() or [''])[-1])
    if v == 'plain':
        plain = d
        for l in o.stdout.splitlines():
            if 'VALUE-MISMATCH' in l or 'UNDETECTED-BENIGN' in l:
                print(f'  counterexample (as designed): {l}', flush=True)
    bad, n = reader_check(d)
    row(f'reader exit codes on the {v} captures', not bad and n > 0, f'({n} captures) {"; ".join(bad[:3])}')

# 4: threaded TSan
d = out / 'tsan'
d.mkdir()
r = run([cc, *base, '-fsanitize=thread', src / 'prg.c', here / 'rsx_threads.c', '-o', d / 'r1t'], d / 'build.log')
if r.returncode == 0:
    env = dict(os.environ, TSAN_OPTIONS='halt_on_error=0 exitcode=66')
    for sc in ('run', 'seal-live', 'detach'):
        r = run(['setarch', 'x86_64', '-R', d / 'r1t', d, sc], d / f'{sc}.log', env=env)
        ok = r.returncode == 0 and 'ThreadSanitizer' not in r.stderr
        row(f'tsan {sc} report-free', ok, (r.stdout.strip().splitlines() or [''])[-1][:140])
    nl = d / 'nolock'
    nl.mkdir()
    r = run(['setarch', 'x86_64', '-R', d / 'r1t', nl, 'nolock'], nl / 'nolock.log', env=env)
    row('tsan negative control (no snap lock) DETECTED', 'WARNING: ThreadSanitizer: data race' in r.stderr)
    o = oracle(d, 'threads', d / 'oracle.log')
    row('oracle on the threaded captures', o.returncode == 0, (o.stdout.strip().splitlines() or [''])[-1])
    bad, n = reader_check(d)
    row('reader exit codes on the threaded captures', not bad and n == 3, f'({n} captures) {"; ".join(bad[:3])}')
else:
    row('tsan build', False)

# 5: mutants: (name, [(old, new), ...], kill token); the token names the trace whose claim or oracle verdict kills it
core = (src / 'prg.c').read_text(encoding='utf-8')
mutants = [
    ('deferral-falls-back', [('        *done = issued;\n', '        *done = completed;\n')], 't_label_beyond_issued:'),
    ('no-deferral', [('    if (dd <= d->report_count) {', '    if (0) {')], 't_deferral:'),
    ('no-poison-at-attach', [('        d->port.write_timer(d->port.ctx, report_first + i, PRG_POISON);',
                              '        (void)0;')], 't_first_use_stale:'),
    ('slot-reuse-before-drain', [('    if (t - LD_ACQ(&d->drained_pub) >= d->report_count) {', '    if (0) {')],
     't_full_ring:'),
    ('final-from-drained', [('    le(b + 20, d->events_accepted, 8);', '    le(b + 20, d->drained, 8);')],
     'r1-calibrate:'),
    ('no-credit-rule', [('    if (d->p_open_count && d->p_rec_head - LD_ACQ(&d->rec_tail) >= PRG_INTERVALS) {',
                         '    if (0) {')], 't_intervals:'),
    ('seal-releases', [('    ST_REL(&d->state, PRG_SEALED);', '    ST_REL(&d->state, PRG_RELEASED);')],
     't_seal_inflight:'),
    # 1. the fault policy
    ('ext-lock-failure-ignored', [(
        'static int ext_lock(prg_dom *d) { return d->port.ext_lock(d->port.ctx) == 0 ? 0 : (fault(d), -1); }',
        'static int ext_lock(prg_dom *d) { (void)d->port.ext_lock(d->port.ctx); return 0; }')],
     'r1-fault-seal-elock'),
    ('unlock-seam-ignored', [(
        'static int snap_unlock(prg_dom *d) { return d->port.snap_unlock(d->port.ctx) == 0 ? 0 : (fault(d), -1); }',
        'static int snap_unlock(prg_dom *d) { (void)d->port.snap_unlock(d->port.ctx); return 0; }')],
     'r1-fault-producer-sunlock'),
    ('finalize-from-fabricated-snapshot', [(
        '    if (snap_copy_out(d, &s) != 0)\n        return -1;\n    drain_once(d, &s, 1);',
        '    if (snap_copy_out(d, &s) != 0) {\n        memset(&s, 0, sizeof s);\n        s.seq = d->next_emit;\n'
        '    }\n    drain_once(d, &s, 1);')], 'r1-fault-seal-slock'),
    # 2. validate before commit
    ('no-second-label-check', [(
        '    if (label_window(d, L2, d->completed, s->ticket, &done2, &dist2) != 0 || done2 < done1 || dist2 < dist1) {',
        '    if (0) {')],
     'r1-l2-reject-flush'),
    ('emit-before-validation', [('        last = x->timer;\n',
                                 '        last = x->timer;\n        stage_event(d, &x->m, x->timer);\n')],
     'r1-l2-reject-flush'),
    ('free-before-validation', [('        last = x->timer;\n',
                                 '        last = x->timer;\n        ST_REL(&d->drained_pub, d->drained + n + 1);\n')],
     'r1-small-ring-refill'),
    # 3. calibration
    ('cal-eclock-as-timeout', [('            outcome = PRG_CAL_CANCELLED;\n            rc = PRG_ECLOCK;',
                                '            outcome = PRG_CAL_CANCELLED;\n            rc = PRG_ETIMEDOUT;')],
     't_cal_clock_fail:'),
    ('cal-second-label-unchecked', [(
        '        if ((uint32_t)(d->port.read_label(d->port.ctx, d->label) - (uint32_t)base) != span ||\n'
        '            b.timer == PRG_POISON) {', '        if (b.timer == PRG_POISON) {')], 't_cal_bad_second_label:'),
    ('cal-bad-not-latched', [('            if (c != PRG_CAL_CONSUMED && c != PRG_CAL_CANCELLED) {', '            if (0) {')],
     't_cal_poison:'),
    ('cal-live-bracket-head', [('    emit_brackets(d, s.br_head);\n    state =', '    emit_brackets(d, d->p_br_head);\n'
                                '    state =')], 't_cal_frontier_frozen:'),
    ('cal-no-finalization-cancel', [('if (c == PRG_CAL_ACTIVE && final && CAS(', 'if (0 && final && CAS(')],
     't_cal_seal_during:'),
    ('cal-tb-order-unchecked', [('            if (b.tb1 <= b.tb0 || b.tb0 < d->p_last_tb1)', '            if (0)')],
     't_cal_tb_order:'),
    # 4. pairing
    ('end-ignores-begin-desc', [(' || top->desc != desc) {', ') {')], 't_pair_desc:'),
    ('mark-depth-clamped', [('    if (d->p_depth >= PRG_DEPTH) { /* inside suppressed nesting', '    if (0) { /* inside')],
     't_mark_depth64:'),
    ('suppressed-any-order', [(
        '        if (!d->p_suppressed || (tok->depth & 0x7fffffffu) != d->p_suppressed ||\n'
        '            tok->begin_seq != d->p_sup_stack[d->p_suppressed - 1]) {', '        if (!d->p_suppressed) {')],
     't_pair_suppressed:'),
    # 5. aggregate bounds
    ('no-loss-credit', [(
        '    } else if (d->loss_exhausted || (uint64_t)g->loss_used + g->loss_reserved >= g->loss_max) {',
        '    } else if (0) {')], 't_loss_budget_over:'),
    ('no-terminal-reservation', [('    if ((uint64_t)ledger->loss_used + ledger->loss_reserved >= ledger->loss_max)',
                                  '    if (0)')], 't_attach_bounds:'),
    ('no-epoch-bracket-budget', [('d->br_epoch_used >= g->br_epoch_max || ', '')], 'r1-cal-epoch-over:'),
    ('no-file-bracket-budget', [(' || g->br_file_used >= g->br_file_max) {', ') {')], 'r1-cal-file-over:'),
    ('report-range-unchecked', [('report_first > UINT32_MAX - report_count || ', '')], 't_attach_bounds:'),
    # the r1c3 carries (codex 12:24)
    ('exhaustion-keeps-admitting', [(' || LD_ACQ(&d->exhausted) ||', ' ||')], 't_exhaust_admitted:'),
    ('exhaustion-finalized', [(': s.overflow || d->loss_exhausted ? FIN_OVERFLOWED', ': s.overflow ? FIN_OVERFLOWED')],
     't_exhaust_at_seal:'),
    ('ledger-ceiling-unchecked', [('    if (ledger->loss_max > PRG_LOSS_CHUNKS_MAX || ', '    if (0 && ')],
     't_attach_bounds:'),
    ('l2-capped-compare-only', [(' || done2 < done1 || dist2 < dist1) {', ' || done2 < done1) {')],
     'r1-l2-back-beyond-issued:'),
    ('suppressed-level-as-identity', [(
        """        if (!d->p_suppressed || (tok->depth & 0x7fffffffu) != d->p_suppressed ||
            tok->begin_seq != d->p_sup_stack[d->p_suppressed - 1]) {""",
        """        if (!d->p_suppressed || (tok->depth & 0x7fffffffu) != d->p_suppressed) {""")],
     't_pair_suppressed_stale:'),
    # R2 core additions (codex GO 12:53)
    ('hold-ignored', [('        if (d->port.emittable && !d->port.emittable(d->port.ctx, &x->m)) {', '        if (0) {')],
     't_hold_release:'),
    ('hold-skips-instead-of-stopping', [(
        '            d->st.held++; /* HOLD: the prefix stops here; the slot stays owned; next pass retries */\n'
        '            break;', '            d->st.held++;\n            continue;')], 't_hold_release:'),
    ('scratch-split-ignored', [('d->ev_n == d->scratch_events ||', 'd->ev_n == PRG_EVENTS_MAX ||')],
     't_scratch_split:'),
]
for name, edits, kill in mutants:
    m = core
    if not all(m.count(old) == 1 for old, _ in edits):
        row(f'mutant {name} applies exactly once', False)
        continue
    for old, new in edits:
        m = m.replace(old, new)
    d = out / f'mut-{name}'
    d.mkdir()
    shutil.copy(src / 'prg.h', d / 'prg.h')
    (d / 'prg.c').write_text(m, encoding='utf-8')
    r = run([cc, '-std=c11', '-O1', '-g', '-pthread', f'-I{d}', d / 'prg.c', here / 'rsx_model.c', '-o', d / 'r1m'],
            d / 'build.log')
    if r.returncode != 0:
        row(f'mutant {name} builds', False)
        continue
    (d / 'out').mkdir()
    r = run(['timeout', '120', d / 'r1m', d / 'out'], d / 'run.log')
    o = oracle(d / 'out', 'traces', d / 'oracle.log')
    fails = [l for l in r.stdout.splitlines() if l.startswith('R1 FAIL')] + \
            [l for l in o.stdout.splitlines() if 'ORACLE FAIL' in l]
    hit = [l for l in fails if kill in l]
    row(f'mutant {name} KILLED by {kill.rstrip(":")}', bool(hit), f'({hit[0][:110]})' if hit else '')


# 6: oracle damage controls
def rd_frames(b):
    o, fr = 16, []
    while o < len(b):
        t, ln, _ = struct.unpack_from('<IIQ', b, o)
        fr.append((t, b[o + 16:o + 16 + ln]))
        o += 16 + ln + 4
    return b[:16], fr


def wr_frames(p, hdr, fr):
    w = bytearray(hdr)
    for i, (t, pl) in enumerate(fr):
        s = len(w)
        w += struct.pack('<IIQ', t, len(pl), i) + pl
        w += struct.pack('<I', zlib.crc32(w[s:]))
    p.write_bytes(bytes(w))


def dmg_swap(dd):  # r1-full-ring: events 0..3, LOSS [4, 6], events 10 -> the LOSS after the event that follows it
    p = dd / 'r1-full-ring.prof'
    hdr, fr = rd_frames(p.read_bytes())
    i = next(k for k, (t, _) in enumerate(fr) if t == 15)
    j = next(k for k in range(i + 1, len(fr)) if fr[k][0] == 14)
    fr[i], fr[j] = fr[j], fr[i]
    wr_frames(p, hdr, fr)


def dmg_drop_final(dd):
    p = dd / 'r1-steady.prof'
    hdr, fr = rd_frames(p.read_bytes())
    wr_frames(p, hdr, [f for f in fr if f[0] != 16])


def dmg_not_executed(dd):
    p = dd / 'r1-steady.truth'
    ls = p.read_text().splitlines()
    f = ls[0].split()
    f[5] = '0'
    ls[0] = ' '.join(f)
    p.write_text('\n'.join(ls) + '\n')


def dmg_crc(dd):
    p = dd / 'r1-steady.prof'
    b = bytearray(p.read_bytes())
    b[-10] ^= 1
    p.write_bytes(bytes(b))


def append(name, text):
    return lambda dd: open(dd / name, 'a').write(text)


damages = [
    ('delete-truth', 'r1-steady', lambda dd: (dd / 'r1-steady.truth').unlink(), 'missing .truth'),
    ('delete-expect', 'r1-steady', lambda dd: (dd / 'r1-steady.expect').unlink(), 'missing .expect'),
    ('duplicate-truth', 'r1-steady',
     lambda dd: append('r1-steady.truth', (dd / 'r1-steady.truth').read_text().splitlines()[0] + '\n')(dd),
     'duplicate truth row'),
    ('unknown-directive', 'r1-steady', append('r1-steady.expect', 'bogus 1\n'), 'unknown directive'),
    ('duplicate-directive', 'r1-steady', append('r1-steady.expect', 'events 50\n'), 'duplicate directive'),
    ('emitted-not-executed', 'r1-steady', dmg_not_executed, 'never executed'),
    ('reordered-chunks', 'r1-full-ring', dmg_swap, 'tiling'),
    ('missing-final', 'r1-steady', dmg_drop_final, 'FINAL present False'),
    ('bad-crc', 'r1-steady', dmg_crc, 'CRC mismatch'),
    ('invalid-noreader-exemption', 'r1-steady', append('r1-steady.expect', 'noreader\n'), 'noreader exemption'),
    ('non-boolean-directive', 'r1-steady', lambda dd: (dd / 'r1-steady.expect').write_text(
        (dd / 'r1-steady.expect').read_text().replace('final_written 1', 'final_written 2')), 'boolean'),
    ('extra-trace', 'r1-bogus', lambda dd: [shutil.copy(dd / f'r1-steady{x}', dd / f'r1-bogus{x}')
                                            for x in ('.prof', '.truth', '.expect')], 'not in the pinned'),
]
if plain is None:
    row('oracle damage controls', False, '(no plain captures)')
else:
    dm = out / 'damage'
    dm.mkdir()
    ctl = dm / 'control'
    ctl.mkdir()
    for f in plain.iterdir():
        if f.suffix in ('.prof', '.truth', '.expect'):
            shutil.copy(f, ctl / f.name)
    o = oracle(ctl, 'traces', dm / 'control.log')
    row('oracle damage control: the undamaged copy passes', o.returncode == 0)
    for name, stem, fn, why in damages:
        dd = dm / name
        shutil.copytree(ctl, dd)
        fn(dd)
        o = oracle(dd, 'traces', dm / f'{name}.log')
        failing = [l for l in o.stdout.splitlines() if 'ORACLE FAIL' in l]
        ok = o.returncode == 1 and len(failing) == 1 and failing[0].startswith(stem + ':') and why in failing[0]
        row(f'oracle damage {name} REJECTED ({why})', ok, f'({failing[0][:110] if failing else "no failure"})')

# 7: no allocation
d = out / 'alloc'
d.mkdir()
r = run([cc, '-std=c11', '-O2', '-c', src / 'prg.c', '-o', d / 'prg.o'], d / 'build.log')
u = run(['nm', '-u', d / 'prg.o'], d / 'nm.txt')
heap = [s for s in ('malloc', 'calloc', 'realloc', 'free', 'posix_memalign', 'aligned_alloc') if re.search(
    rf'\b{s}\b', u.stdout)]
row('no allocation: the core references no heap function', r.returncode == 0 and u.returncode == 0 and not heap,
    f'(undefined: {" ".join(l.split()[-1] for l in u.stdout.splitlines())})')

failed = [n for n, ok in rows if not ok]
print(f'r1-test: {"FAIL " + "; ".join(failed) if failed else "PASS"} ({len(rows)} rows)', flush=True)
sys.exit(1 if failed else 0)
