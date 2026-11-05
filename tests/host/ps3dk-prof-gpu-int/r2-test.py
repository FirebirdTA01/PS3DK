#!/usr/bin/env python3
"""P3.3 slice R2: GPU (RSX) domains inside the runtime, HOST ONLY (design 935936fb + deltas 2400e50a, 0e23d13e, the
review 12:53 rulings and the 14:03 corrections).  The GPU device is an in-order emulator behind the test-hook port:
host-model evidence, not target evidence; A1/A2 remain assumptions; the mapping is UNMEASURED.

Rows (exit 0 only if all pass):
  1. every case of gpu_int.c, plain and ASan/UBSan (one process each): its in-process claims (PASS line, rc 0);
  2. the committed reader's exact exit code on every named capture (per .expect), and `unresolved_frames N` against
     the reader's own frame-resolution line;
  3. the fail-closed checker (gpu_oracle.py, pinned inventory and pinned exemptions) on the named captures; the SPU
     source of the mixed capture through the I2 SPU oracle with its per-lane truth;
  4. oracle damage controls: each damaged copy fails with its named reason and nothing else;
  5. TSan: the owner-churn run, the mixed SPU+GPU run and the THREAD-mode detach overlap, report-free; the NEGATIVE
     control (the snap mutex disabled) reported;
  6. the off build: C and C++, PS3DK_PROF=0, RUN;
  7. runtime/core mutants, each KILLED by its NAMED assertion: a FAIL line of its case containing the named text, the
     reader or the checker failing on that case's named capture, or (designated only) the case's watchdog.  A crash,
     a loader/build failure or an unrelated timeout INVALIDATES the row (never a kill); harness-failure controls prove
     the classifier;
  8. the R1 core gates (tests/host/ps3dk-prof-rsx/r1-test.py) on the changed core.

usage: r2-test.py OUT --reader READER"""
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
lib = root / 'sdk/libps3dkprof/src'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
rows = []
REJECT = ['gpu-5', 'slots-1', 'slots-1025', 'slots-without-gpu', 'reserved-0', 'reserved-1', 'ring-without-spu',
          'image-without-spu', 'mem-without-ext']
CASES = [f'cfg-reject-{r}' for r in REJECT] + '''cfg-combo-off cfg-combo-spu cfg-combo-gpu cfg-combo-both cfg-init-fail
    init-unwind-clock init-unwind-sink init-unwind-thread api-refusals lifetime steady two-owner desc-hold orphan tags
    lease sink-fail-descriptors sink-fail-events sink-fail-final detach-timeout-seal sched-stale
    sched-shutdown-after-check sched-shutdown-in-reserve detach-inline detach-overlap detach-app-helper gpu-exhaust
    mixed-spu mixed-corrupt dropped-frame snap-first-lock snap-first-unlock frame-thread-lock frame-thread-unlock
    spu-reentry fault-attach-lock fault-attach-unlock fault-detach-unlock fault-release-unlock fault-seal-unlock
    fault-snap-lock fault-snap-unlock fault-stats-unlock completion-held completion-fail completion-unlock
    detach-seal-wins frame-unlock-invalid frame-unlock-busy ack-lifetime-1 ack-lifetime-2'''.split()
TSAN_CASES = ['tsan-run', 'mixed-tsan', 'detach-overlap', 'completion-held', 'completion-unlock', 'ack-lifetime-1',
              'ack-lifetime-2']
WATCHDOG = -14  # SIGALRM: the designated watchdog cases call alarm()


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def run(cmd, log, timeout=180, **kw):
    try:
        r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        Path(log).write_text(' '.join(map(str, cmd)) + '\n\nTIMEOUT\n')
        return None
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + r.stderr)
    return r


def build(d, src_root, extra):
    s = src_root
    inc = d / 'inc'
    (inc / 'ps3dk').mkdir(parents=True, exist_ok=True)
    shutil.copy(root / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
    flags = ['-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=1',
             '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DPS3DK_PROF_WITH_SPU', f'-I{inc}', f'-I{s}/core',
             f'-I{s}/pal', f'-I{s}/spu', f'-I{s}/rsx', *extra]
    srcs = [s / f for f in ('core/prof_core.c', 'core/prof_drain.c', 'pal/pal_host.c', 'spu/psp_ppu.c',
                            'spu/psp_spu.c', 'spu/ext_runtime.c', 'spu/psp_elf.c', 'spu/psp_host_emul.c',
                            'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/gpu_port_gcm.c')] + [here / 'gpu_int.c']
    r = run([cc, *flags, *srcs, '-o', d / 'gpu'], d / 'build.log')
    return r is not None and r.returncode == 0


def run_case(exe, d, case, arg=None, env=None, prefix=()):
    """(status, fail_lines, dir, stdout, rc): status PASS, FAIL (the case reported a FAIL line and exited 1),
    WATCHDOG (killed by its own alarm), or INVALID (crash, timeout, missing verdict line)."""
    cd = d / f'run-{case}'
    cd.mkdir()
    r = run([*prefix, exe, case, cd] + ([arg] if arg else []), cd / 'log.txt', env=env, timeout=300)
    if r is None:
        return 'INVALID', ['TIMEOUT'], cd, '', None
    fails = [l for l in r.stdout.splitlines() if l.startswith(f'R2 {case} FAIL')]
    san = 'runtime error' in r.stderr or 'AddressSanitizer' in r.stderr
    if r.returncode == 0 and f'R2 {case} PASS' in r.stdout and not san and not fails:
        return 'PASS', [], cd, r.stdout, 0
    if r.returncode == 1 and fails and not san:
        return 'FAIL', fails, cd, r.stdout, 1
    if r.returncode == WATCHDOG:
        return 'WATCHDOG', [], cd, r.stdout, r.returncode
    return 'INVALID', fails or [f'rc {r.returncode}'], cd, r.stdout, r.returncode


def collect(case_dirs, cap):
    cap.mkdir()
    for cd in case_dirs:
        for t in cd.glob('*.truth'):
            for ext in ('.prof', '.truth', '.expect', '.spu-truth', '.spu-expect'):
                f = t.with_suffix(ext)
                if f.exists():
                    shutil.copy(f, cap / f.name)


def reader_one(prof, expect):
    lines = expect.read_text().splitlines()
    want = [l.split()[1] for l in lines if l.startswith('exit ')]
    r = subprocess.run([str(a.reader), 'check', str(prof)], capture_output=True, text=True)
    bad = []
    if len(want) != 1 or str(r.returncode) != want[0]:
        bad.append(f'{prof.stem}: rc {r.returncode} want {want}')
    for l in lines:
        if l.startswith('unresolved_frames '):
            m = re.search(r'frames resolved \d+ unresolved (\d+)', r.stdout)
            if not m or m.group(1) != l.split()[1]:
                bad.append(f'{prof.stem}: unresolved frames {m.group(1) if m else "?"} want {l.split()[1]}')
    return bad


def reader_check(cap):
    bad, n = [], 0
    for e in sorted(cap.glob('*.expect')):
        n += 1
        bad += reader_one(e.with_suffix('.prof'), e)
    return bad, n


def oracle(cap, suite, log):
    return run([sys.executable, '-I', here / 'gpu_oracle.py', cap, '--suite', suite], log)


san_env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=0')
plain_cap = None

# 1-3: every case, plain and asan
for v, extra in (('plain', []), ('asan', ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'])):
    d = out / v
    d.mkdir()
    if not build(d, lib, extra):
        row(f'build ({v})', False)
        continue
    dirs, failed = [], []
    for c in CASES:
        st, why, cd, _, _ = run_case(d / 'gpu', d, c, env=san_env)
        dirs.append(cd)
        if st != 'PASS':
            failed.append(f'{c}: {st} {"; ".join(why[:1])}')
    st, why, cd, so, _ = run_case(d / 'gpu', d, 'cfg-cap-probe', env=san_env)
    m = re.search(r'R2 SUBTOTAL (\d+)', so)
    if st != 'PASS' or not m:
        failed.append('cfg-cap-probe')
    else:
        for c in ('exact', 'short'):
            st, why, cd, _, _ = run_case(d / 'gpu', d, f'cfg-cap-{c}', m.group(1), env=san_env)
            if st != 'PASS':
                failed.append(f'cfg-cap-{c}: {st}')
    row(f'cases ({v})', not failed, f'({len(CASES) + 3} cases) {"; ".join(failed[:4])}')
    cap = d / 'captures'
    collect(dirs, cap)
    bad, n = reader_check(cap)
    row(f'reader exit codes ({v})', not bad and n > 0, f'({n} captures) {"; ".join(bad[:3])}')
    o = oracle(cap, 'plain', d / 'oracle.log')
    row(f'oracle ({v})', o is not None and o.returncode == 0, (o.stdout.strip().splitlines() or [''])[-1] if o else '')
    if v == 'plain':
        plain_cap = cap

# 3b: the SPU source of the mixed capture, through the I2 SPU oracle with its per-lane truth
if plain_cap is not None:
    target = a.reader.resolve().parent
    so = out / 'spu-oracle'
    so.mkdir()
    r = run(['rustc', '--edition=2021', '-O', root / 'tests/host/ps3dk-prof-spu/spu_model_oracle.rs', '--extern',
             f'ps3dk_prof={target}/libps3dk_prof.rlib', '-L', f'dependency={target}/deps', '-o', so / 'oracle'],
            so / 'build.log', timeout=600)
    sd = so / 'captures'
    sd.mkdir()
    for ext, to in (('.prof', '.prof'), ('.spu-truth', '.truth'), ('.spu-expect', '.expect')):
        shutil.copy(plain_cap / f'r2-mixed-spu{ext}', sd / f'r2-mixed-spu{to}')
    o = run([so / 'oracle', sd], so / 'run.log') if r is not None and r.returncode == 0 else None
    row('SPU source of the mixed capture: per-lane truth (I2 SPU oracle)', o is not None and o.returncode == 0,
        (o.stdout.strip().splitlines() or [''])[-1] if o else 'oracle build failed')


# 4: oracle damage controls
def rd_frames(b):
    o, fr = 16, []
    while o < len(b):
        t, ln, _ = struct.unpack_from('<IIQ', b, o)
        fr.append((t, bytearray(b[o + 16:o + 16 + ln])))
        o += 16 + ln + 4
    return b[:16], fr


def wr_frames(p, hdr, fr):
    w = bytearray(hdr)
    for i, (t, pl) in enumerate(fr):
        s = len(w)
        w += struct.pack('<IIQ', t, len(pl), i) + pl
        w += struct.pack('<I', zlib.crc32(w[s:]))
    p.write_bytes(bytes(w))


def dmg_final_counts(dd):
    p = dd / 'r2-steady.prof'
    hdr, fr = rd_frames(p.read_bytes())
    i = next(k for k, (t, _) in enumerate(fr) if t == 16)
    struct.pack_into('<Q', fr[i][1], 4 + 16, struct.unpack_from('<Q', fr[i][1], 4 + 16)[0] + 1)  # events_total
    wr_frames(p, hdr, fr)


def dmg_events_chunk(dd, dup):
    p = dd / 'r2-lease.prof'
    hdr, fr = rd_frames(p.read_bytes())
    i = next(k for k, (t, _) in enumerate(fr) if t == 14)
    fr = fr[:i + 1] + [fr[i]] + fr[i + 1:] if dup else fr[:i] + fr[i + 1:]
    wr_frames(p, hdr, fr)


def dmg_truncate_truth(dd):
    p = dd / 'r2-steady.truth'
    p.write_text(''.join(p.read_text().splitlines(keepends=True)[:-1]))


def append(name, text):
    return lambda dd: open(dd / name, 'a').write(text)


damages = [
    ('final-counts', 'r2-steady', dmg_final_counts, 'FINAL events/lost'),
    ('missing-events-chunk', 'r2-lease', lambda dd: dmg_events_chunk(dd, False), 'tiling'),
    ('duplicate-events-chunk', 'r2-lease', lambda dd: dmg_events_chunk(dd, True), 'tiling'),
    ('truncated-truth', 'r2-steady', dmg_truncate_truth, 'numbered calls'),
    ('illicit-exemption', 'r2-steady', append('r2-steady.expect', 'preexisting_malformed 1\n'), 'not permitted'),
    ('illicit-sink-dead', 'r2-lease', append('r2-lease.expect', 'sink_dead 1\n'), 'not permitted'),
    ('extra-capture', 'r2-bogus', lambda dd: [shutil.copy(dd / f'r2-steady{x}', dd / f'r2-bogus{x}')
                                              for x in ('.prof', '.truth', '.expect')], 'not in the pinned'),
    ('deleted-truth', 'r2-tags', lambda dd: (dd / 'r2-tags.truth').unlink(), 'missing .truth'),
]
if plain_cap is None:
    row('oracle damage controls', False, '(no plain captures)')
else:
    dm = out / 'damage'
    dm.mkdir()
    ctl = dm / 'control'
    shutil.copytree(plain_cap, ctl)
    o = oracle(ctl, 'plain', dm / 'control.log')
    row('oracle damage control: the undamaged copy passes', o is not None and o.returncode == 0)
    for name, stem, fn, why in damages:
        dd = dm / name
        shutil.copytree(ctl, dd)
        fn(dd)
        o = oracle(dd, 'plain', dm / f'{name}.log')
        failing = [l for l in (o.stdout.splitlines() if o else []) if 'ORACLE FAIL' in l]
        ok = o is not None and o.returncode == 1 and len(failing) == 1 and failing[0].startswith(stem + ':') and \
            why in failing[0]
        row(f'oracle damage {name} REJECTED ({why})', ok, f'({failing[0][:110] if failing else "no failure"})')

# 5: TSan
d = out / 'tsan'
d.mkdir()
if build(d, lib, ['-fsanitize=thread']):
    env = dict(os.environ, TSAN_OPTIONS='halt_on_error=0 exitcode=66')
    dirs = []
    for c in TSAN_CASES:
        cd = d / f'run-{c}'
        cd.mkdir()
        dirs.append(cd)
        r = run(['setarch', 'x86_64', '-R', d / 'gpu', c, cd], cd / 'log.txt', timeout=900, env=env)
        ok = r is not None and r.returncode == 0 and 'ThreadSanitizer' not in r.stderr and f'R2 {c} PASS' in r.stdout
        row(f'tsan {c} report-free', ok, ([l for l in (r.stdout.splitlines() if r else []) if 'frames ok' in l] or
                                          [''])[0])
    cap = d / 'captures'
    collect(dirs, cap)
    bad, n = reader_check(cap)
    row('reader exit codes (tsan)', not bad and n == len(TSAN_CASES), '; '.join(bad))
    o = oracle(cap, 'tsan', d / 'oracle.log')
    row('oracle (tsan)', o is not None and o.returncode == 0)
    nd = d / 'run-tsan-nolock'
    nd.mkdir()
    r = run(['setarch', 'x86_64', '-R', d / 'gpu', 'tsan-nolock', nd], nd / 'log.txt', timeout=900, env=env)
    row('tsan negative control (snap mutex disabled) DETECTED',
        r is not None and 'WARNING: ThreadSanitizer: data race' in r.stderr)
else:
    row('build (tsan)', False)

# 6: the off build
d = out / 'offbuild'
d.mkdir()
for lang, comp, std in (('c', cc, '-std=c11'), ('c++', os.environ.get('CXX', 'c++'), '-std=c++17')):
    exe = d / f'off-{lang}'
    r = run([comp, '-x', lang, std, '-O1', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=0', f'-I{root}/sdk/include',
             f'-I{lib}/rsx', here / 'offbuild_gpu.c', '-o', exe], d / f'build-{lang}.log')
    ok = r is not None and r.returncode == 0
    if ok:
        r = run([exe], d / f'run-{lang}.log')
        ok = r is not None and r.returncode == 0 and 'PASS' in r.stdout
    row(f'off build ({lang}): no evaluation, no write, ENOTSUP', ok)

# 7: mutants: (name, file, old, new, case, kill, [count]); kill: a FAIL-line text, 'CAPTURE' (the reader or the
# checker fails on the case's named capture) or 'WATCHDOG' (designated)
G = 'rsx/gpu_runtime.c'
E = 'spu/ext_runtime.c'
mutants = [
    ('hold-ignored', G, '    if (m->desc >= p->dr.desc_emitted)\n        return 0;\n', '', 'desc-hold',
     'held at its descriptor'),
    ('discard-absent', G, '    if (p->snk.failed)\n        return 1;\n', '', 'sink-fail-descriptors',
     'detach completes on the discard path'),
    ('orphan-emitted', G, '            g->orphaned++;\n        }\n        return 0;', '            g->orphaned++;\n'
     '        }\n        return 1;', 'orphan', 'ORPHAN-held, never emitted'),
    ('free-scan-removed', E, '        p_gpu_slot_freed(p, slot);', '        (void)0;', 'two-owner',
     '2 events, no hold, no orphan'),
    ('partition-not-applied', 'core/prof_core.c', '            lim.loss_chunks = SPU_LOSS_CHUNKS_WITH_GPU;\n', '',
     'cfg-combo-both', 'the static partition'),
    ('overlap-unchecked', G, '        if (LD_ACQ(&o->state) == GPU_S_FREE)\n            continue;',
     '        continue;', 'api-refusals', 'same ctx'),
    ('id-reused', G, '    id = GPU_ID_BASE + p->gpu_lifetime;', '    id = GPU_ID_BASE;', 'lifetime', 'never reused'),
    ('scope-unchecked', G, '    if (rc == PS3DK_PROF_OK && scope != 2)\n        rc = PS3DK_PROF_EINVAL;\n', '',
     'api-refusals', 'a scope-1 descriptor', 2),
    ('pin-without-id-check', G, '        if (h && LD_SC_(&g->id) == h)', '        if (h)', 'sched-stale',
     'the stale call resumes and misses'),
    ('attach-ignores-pins', G, ' && LD_SC_(&p->gpu[i].users) == 0', '', 'sched-stale', 'the pinned slot was skipped'),
    ('attach-without-lock', G, '    if (p_ext_lock_at(p) != 0) {\n        ST_REL(&p->ext_fault, 1u);\n'
     '        ST_RLX(&p->lock_failed, 1);\n        return PS3DK_PROF_ESTOPPED;\n    }', '    (void)0;',
     'fault-attach-lock', 'attach: nothing attached'),
    ('release-frees-before-unlock', G, '        ST_REL(&g->state, (uint32_t)GPU_S_QUARANTINED); /* not FREE until',
     '        ST_REL(&g->state, (uint32_t)GPU_S_FREE); /* not FREE until', 'fault-release-unlock',
     'the slot is QUARANTINED'),
    ('detach-holds-the-mutex', G, '    rc = map(prg_detach_wait(&g->dom, timeout_us, poll_inline, p));',
     '    (void)ext_enter(p);\n    rc = map(prg_detach_wait(&g->dom, timeout_us, poll_inline, p));\n'
     '    (void)p_ext_unlock_site(p, U_ANY);', 'detach-inline', 'WATCHDOG'),
    ('tag-wraps', G, '    if (o->tag + 1u >= GPU_TAG_MAX)\n        return 0;', '    if (0)\n        return 0;', 'tags',
     'at the last tag'),
    ('lease-ignored', G, '    return !o->cur && o->last_ticket <= LD_ACQ(&g->dom.drained_pub);', '    return !o->cur;',
     'lease', 'all 8 entries leased'),
    ('reentry-allowed', G, '''    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY;
    p = p_ready() ? g_prof : NULL;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max)
        return PS3DK_PROF_ENOTSUP;
    if (LD_ACQ(&p->ext_fault))
        return PS3DK_PROF_ESTOPPED;
    g = pin(p, h, &rc);''', '''    p = p_ready() ? g_prof : NULL;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max)
        return PS3DK_PROF_ENOTSUP;
    if (LD_ACQ(&p->ext_fault))
        return PS3DK_PROF_ESTOPPED;
    g = pin(p, h, &rc);''', 'api-refusals', 'from a port callback'),
    ('owner-not-enforced', G, '    if (!pal_owner_is_self(&g->owner)) {\n        unpin(g);\n        return PS3DK_PROF_EPERM;\n'
     '    }', '', 'api-refusals', 'foreign begin'),
    # the 14:03 corrections
    ('detach-encodes-on-caller', G, '    rc = map(prg_detach_wait(&g->dom, timeout_us, poll_inline, p));',
     '    g->dom.drainer_finalizes = 0;\n    rc = map(prg_detach(&g->dom, timeout_us, poll_inline, p));',
     'detach-overlap', 'the ctx thread never wrote'),
    ('drainer-never-finalizes', 'rsx/prg.c', '        (void)finalize(d); /* -1 (an unproven snapshot)',
     '        (void)0; /* -1 (an unproven snapshot)', 'detach-inline', 'detach completes through its inline drains'),
    ('faulted-domain-undeclared', 'rsx/prg.c', [('''    if (!LD_ACQ(&d->retired) && st != PRG_FREE)
        declare(d);''', '''    if (!faulted(d) && !LD_ACQ(&d->retired) && st != PRG_FREE)
        declare(d);'''), ('''    if (!LD_ACQ(&d->retired) && LD_ACQ(&d->state) != PRG_FREE)
        declare(d); /* as prg_drain_held''', '''    if (!faulted(d) && !LD_ACQ(&d->retired) && LD_ACQ(&d->state) != PRG_FREE)
        declare(d); /* as prg_drain_held''')], None, 'snap-first-lock', 'CAPTURE'),
    ('unwind-keeps-gpu-mutexes', 'core/prof_core.c', '    if (p->gpu_max)\n        p_gpu_destroy(p); /* after',
     '    if (0)\n        p_gpu_destroy(p); /* after', 'init-unwind-clock', '3 initialized, 3 destroyed'),
    ('corruption-forwarded-late', E, '        if (p->spu && p->spu->corrupt)\n            p_ext_sink_fail(p, -2); /* as above: '
     'before any GPU output */\n        if (p->gpu_max)\n            p_gpu_step(p);',
     '        if (p->gpu_max)\n            p_gpu_step(p);\n        if (p->spu && p->spu->corrupt)\n'
     '            p_ext_sink_fail(p, -2);', 'mixed-corrupt', 'no GPU_EVENTS reached the sink'),
    ('frame-unlock-fault-ignored', G, '        if (LD_RLX(&p->lock_failed)) {\n            /* the thread_mutex unlock failed',
     '        if (0) {\n            /* the thread_mutex unlock failed', 'frame-thread-unlock', 'frame: ESTOPPED'),
    ('spu-reentry-allowed', E, '    if (g_tls.in_drain || g_tls.in_gpu)\n        return PS3DK_PROF_EBUSY; /* a sink callback',
     '    if (g_tls.in_drain)\n        return PS3DK_PROF_EBUSY; /* a sink callback', 'spu-reentry', 'WATCHDOG'),
    # the 14:53 corrections
    ('completion-on-retired', 'rsx/prg.c', '''        if (e == PRG_DETACH_FINAL) {
            uint32_t ack = LD_ACQ(&d->detach_ack); /* not `retired` alone: that is set before FINAL is offered */
            if (ack == 1 && LD_ACQ(&d->retired))
                return PRG_OK; /* the acknowledged turn AND the frozen decision */
            if (ack == 2)
                return PRG_EFAULT;
        }''', '''        if (e == PRG_DETACH_FINAL && LD_ACQ(&d->retired))
            return PRG_OK;''', 'completion-held', 'pending: ETIMEDOUT'),
    ('ack-before-unlock', 'spu/ext_runtime.c', '''        int ok = ext_unlock_at(p, seal ? U_SEAL : U_STEP3B) == 0;''',
     '''        int ok;
        p_gpu_ack_publish(p, acks, nack, 1);
        nack = 0;
        ok = ext_unlock_at(p, seal ? U_SEAL : U_STEP3B) == 0;''', 'completion-unlock', "the turn's closing unlock failed"),
    # the 16:11 correction: the acknowledgement's lifetime
    ('release-ignores-pending-ack', G, '''    if (LD_ACQ(&g->dom.state) == PRG_DETACH_FINAL && LD_ACQ(&g->dom.detach_ack) == 0) {''',
     '''    if (0) {''', 'ack-lifetime-1', 'release while the acknowledgement is pending'),
    # (an 'ack-scan-after-unlock' TSan-only mutant was tried and NOT detected by TSan in ack-lifetime-2: it is not
    # claimed as evidence; the collection under the lock is structural and the lifetime rule is mutated above)
    ('frame-invalid-unlock-ignored', G, '''            return done(g, LD_RLX(&p->lock_failed) ? PS3DK_PROF_ESTOPPED : PS3DK_PROF_EINVAL);''',
     '''            return done(g, PS3DK_PROF_EINVAL);''', 'frame-unlock-invalid', 'a failed closing unlock on the refusal'),
    ('frame-busy-unlock-ignored', G, '''                if (LD_RLX(&p->lock_failed))
                    return done(g, PS3DK_PROF_ESTOPPED); /* terminal; the current owner and every lease unchanged */''',
     '''                if (0)
                    return done(g, PS3DK_PROF_ESTOPPED);''', 'frame-unlock-busy', 'a failed closing unlock on the refusal'),
    ('stats-unlock-ignored', G, '    if (p_ext_unlock_site(p, U_GPU_STATS) != 0)\n        return done(g, PS3DK_PROF_ESTOPPED);',
     '    (void)p_ext_unlock_site(p, U_GPU_STATS);', 'fault-stats-unlock', 'stats: ESTOPPED, nothing written'),
]


def capture_fails(cd, case):
    """the reader or the checker failing on the case's named capture(s)"""
    caps = [t.stem for t in cd.glob('*.truth')]
    if not caps:
        return []
    tmp = cd / 'oracle-cap'
    tmp.mkdir()
    bad = []
    for stem in caps:
        e = cd / f'{stem}.expect'
        if e.exists():
            bad += reader_one(cd / f'{stem}.prof', e)
    for f in cd.iterdir():
        if f.suffix in ('.prof', '.truth', '.expect') and f.stem in caps:
            shutil.copy(f, tmp / f.name)
    # the checker on the named capture alone: its inventory complaint about the other names is expected; only lines
    # for this capture count
    o = run([sys.executable, '-I', here / 'gpu_oracle.py', tmp, '--suite', 'plain'], cd / 'oracle.log')
    bad += [l for l in (o.stdout.splitlines() if o else []) if 'ORACLE FAIL' in l and
            any(l.startswith(s + ':') and 'missing' not in l for s in caps)]
    return bad


# harness-failure controls: the classifier never counts a crash, an unknown case or a timeout as a kill
d = out / 'controls'
d.mkdir()
if build(d, lib, []):
    for case, want in (('crash-control', 'INVALID'), ('no-such-case', 'INVALID')):
        st, why, cd, _, _ = run_case(d / 'gpu', d, case)
        row(f'harness control {case} classified {want} (never a kill)', st == want, f'(got {st})')
else:
    row('build (controls)', False)

for name, f, old, new, case, kill, *n in mutants:
    src = (lib / f).read_text(encoding='utf-8')
    edits = old if isinstance(old, list) else [(old, new)]  # several sites of one mutation, each exactly once
    want = n[0] if n else 1
    if any(src.count(o) != (want if len(edits) == 1 else 1) for o, _ in edits):
        row(f'mutant {name} applies exactly as specified', False, f'(counts {[src.count(o) for o, _ in edits]})')
        continue
    for o, nw in edits:
        src = src.replace(o, nw)
    d = out / f'mut-{name}'
    d.mkdir()
    shutil.copytree(lib, d / 'src')
    (d / 'src' / f).write_text(src, encoding='utf-8')
    tsan = kill == 'TSAN'
    if not build(d, d / 'src', ['-Wno-error', '-Wno-unused-variable', '-Wno-unused-function'] +
                 (['-fsanitize=thread'] if tsan else [])):
        row(f'mutant {name}: INVALID (build failed)', False)
        continue
    if tsan:  # killed only by a TSan data-race report from its named case (designated)
        cd = d / f'run-{case}'
        cd.mkdir()
        r = run(['setarch', 'x86_64', '-R', d / 'gpu', case, cd], cd / 'log.txt', timeout=900,
                env=dict(os.environ, TSAN_OPTIONS='halt_on_error=0 exitcode=66'))
        hit = r is not None and 'WARNING: ThreadSanitizer: data race' in r.stderr
        row(f'mutant {name} KILLED by {case} (TSAN)', hit, '(data race reported)' if hit else '(no report)')
        continue
    st, fails, cd, so, rc = run_case(d / 'gpu', d, case)
    if kill == 'WATCHDOG':
        killed, how = st == 'WATCHDOG', f'{st} rc {rc}'
    elif kill == 'CAPTURE':
        cf = capture_fails(cd, case) if st in ('PASS', 'FAIL') else []
        killed, how = bool(cf), (cf[0] if cf else st)
    else:
        hit = [l for l in fails if kill in l]
        killed, how = st == 'FAIL' and bool(hit), (hit[0] if hit else f'{st} {"; ".join(fails[:1])}')
    row(f'mutant {name} KILLED by {case} ({kill})', killed, f'({how[:110]})')

# 8: the R1 core gates on the changed core
r = run([sys.executable, '-I', '-B', root / 'tests/host/ps3dk-prof-rsx/r1-test.py', out / 'r1', '--reader', a.reader],
        out / 'r1.log', timeout=1800)
row('R1 core gates (r1-test.py)', r is not None and r.returncode == 0,
    (r.stdout.strip().splitlines() or [''])[-1] if r else 'TIMEOUT')

failed = [n for n, ok in rows if not ok]
print(f'r2-test: {"FAIL " + "; ".join(failed) if failed else "PASS"} ({len(rows)} rows)', flush=True)
sys.exit(1 if failed else 0)
