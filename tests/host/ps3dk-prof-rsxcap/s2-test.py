#!/usr/bin/env python3
"""Capture RSX frame B1, slice S2: the runtime core, HOST ONLY.  Host-model evidence, not target evidence.

Groups (exit 0 only if every row passes):
  core  rsxcap_core.c: prg.c standalone over an emulator port (receipts, prg_frame_capture's one admission,
        prg_zone_abandon, the fault reason, the seal at P1-P6), plain and non-recovering ASan/UBSan;
  rt    rsxcap_rt.c: the REAL runtime + the REAL GCM port over fake memory (the production submit path), one case
        per process, plain and ASan/UBSan; each capture-writing case leaves <name>.prof + <name>.expect;
  tsan  the rt cases (all but the four longest) and the core seal / fault-reason cases under -fsanitize=thread (the
        rt build with the SC device-memory seam), no suppressions; the two NEGATIVE controls must report a data race
        in their named function;
  ct    rsxcap_contract.c: the formatter, emitter, kind table and pool against the frozen fixture; the fixture pin;
        the pool layout measured from the object;
  rd    the committed reader: `ps3dk-prof check` exit codes, the round-trip checker (rsxcap_check.rs) over every
        build's captures, and damaged copies that must fail with their named reason;
  mut   source mutants, each KILLED by its named case (a FAIL line of that case, or the checker failing on its
        capture); a crash, a timeout or a build failure INVALIDATES the row (never a kill);
  reg   regressions and hygiene: r2 (with R1), r4a, cargo rsx_capture, the ext-target checker self-test, archive
        symbols, shipped members, no heap, no timers, loop bounds, input pins, harness controls.

Every wait in the harnesses is an iteration-bounded spin (HARNESS-STALL, exit 3: INVALID); the subprocess timeout
here is a harness watchdog (HARNESS-TIMEOUT: INVALID), never a PASS.

usage: s2-test.py OUT --reader READER --rlib RLIB [--skip-regress]"""
import argparse
import hashlib
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
ap.add_argument('--rlib', type=Path, required=True)
ap.add_argument('--skip-regress', action='store_true')
ap.add_argument('--only', default='')
ap.add_argument('--diff', type=Path, help='git diff HEAD -- sdk/ (produced where git can read the worktree)')
ap.add_argument('--tree-scan', type=Path, help='the output of neutral-wording-scan.py --tree, run where git works')
a = ap.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
lib = root / 'sdk/libps3dkprof/src'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
FIXTURE = root / 'tools/ps3dk-prof/tests/fixtures/rsx_capture_contract.txt'
FIXTURE_SHA = '853cfdcaa0d038e51903835b02bb9448f1fdb2c4deb2f6bd1562fceb524970b2'
READER_SRC = root / 'tools/ps3dk-prof/src/rsx_capture.rs'
READER_SHA16 = '57782a3786b03916'
rows = []  # (group, name, status) status: PASS FAIL INVALID


def row(group, name, ok, info='', status=None):
    st = status or ('PASS' if ok else 'FAIL')
    rows.append((group, name, st))
    print(f'{group}.{name}: {st} {info}'.rstrip(), flush=True)


def run(cmd, log, timeout=600, env=None, cwd=None):
    try:
        r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, env=env, cwd=cwd)
    except subprocess.TimeoutExpired:
        Path(log).write_text(' '.join(map(str, cmd)) + '\n\nHARNESS-TIMEOUT\n')
        return None
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + '\n--- stderr\n' + r.stderr)
    return r


SAN = {'plain': [], 'asan': ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'],
       'tsan': ['-fsanitize=thread']}
SAN_TEXT = ('runtime error', 'AddressSanitizer', 'ThreadSanitizer', 'LeakSanitizer', 'UndefinedBehaviorSanitizer')
RT_SRCS = ('core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c', 'spu/psp_ppu.c', 'spu/psp_spu.c',
           'spu/ext_runtime.c', 'spu/psp_elf.c', 'spu/psp_host_emul.c', 'rsx/gpu_runtime.c', 'rsx/prg.c',
           'rsx/gpu_port_gcm.c', 'rsx/rsxcap.c')


def build(d, kind, san, src=lib, tdir=here):
    """kind: core | rt | ct.  Returns the executable or None."""
    d.mkdir(parents=True, exist_ok=True)
    inc = d / 'inc'
    (inc / 'ps3dk').mkdir(parents=True, exist_ok=True)
    shutil.copy(root / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
    fl = ['-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror', *SAN[san]]
    if kind != 'ct':
        fl.append('-DPRG_TEST_STEPS')  # the contract harness links the seam-free core
    exe = d / kind
    if kind == 'core':
        cmd = [cc, *fl, f'-I{src}/rsx', src / 'rsx/prg.c', tdir / 'rsxcap_core.c', '-o', exe]
    else:
        fl += ['-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DPS3DK_PROF_WITH_SPU',
               f'-I{inc}', f'-I{src}/core', f'-I{src}/pal', f'-I{src}/spu', f'-I{src}/rsx']
        if san == 'tsan':
            fl.append('-DGPP_HOST_DEVMEM')  # every device-memory access an SC atomic (the R4a Mode C seam)
        harness = tdir / ('rsxcap_rt.c' if kind == 'rt' else 'rsxcap_contract.c')
        cmd = [cc, *fl, *[src / s for s in RT_SRCS], harness, '-o', exe]
    r = run(cmd, d / f'build-{kind}.log', timeout=900)
    return exe if r is not None and r.returncode == 0 else None


def classify(r, prefix, case):
    """PASS / FAIL (rc 1 with a FAIL line, no sanitizer text) / INVALID"""
    if r is None:
        return 'INVALID'
    san = any(t in r.stderr for t in SAN_TEXT)
    fails = [l for l in r.stdout.splitlines() if l.startswith(f'{prefix} {case} FAIL')]
    if r.returncode == 0 and f'{prefix} {case} PASS' in r.stdout and not san and not fails:
        return 'PASS'
    if r.returncode == 1 and fails and not san:
        return 'FAIL'
    return 'INVALID'


TSAN_ENV = dict(os.environ, TSAN_OPTIONS='halt_on_error=0 exitcode=66 report_signal_unsafe=0')
assert 'suppressions' not in TSAN_ENV['TSAN_OPTIONS']


def run_case(exe, d, prefix, case, san, extra=(), timeout=900):
    cd = d / f'run-{case}'
    cd.mkdir(parents=True, exist_ok=True)
    pre = ['setarch', 'x86_64', '-R'] if san == 'tsan' else []
    args = [case, *extra] if prefix != 'RT' else [case, cd]
    r = run([*pre, exe, *args], cd / 'log.txt', timeout=timeout, env=TSAN_ENV if san == 'tsan' else None)
    return classify(r, prefix, case), r, cd


CORE = '''receipt_refused_sealed receipt_refused_detaching receipt_refused_anomaly receipt_refused_overflow
receipt_refused_exhausted receipt_refused_faulted receipt_refused_einval receipt_refused_seq_overflow
receipt_refused_suppress_overflow receipt_refused_mark_depth publish_fail_receipt wrapper_rc fc_admission_accounting
fc_refused_sealing fc_refused_detaching fc_refused_anomaly fc_refused_overflow fc_refused_exhausted fc_refused_faulted
fc_order fc_end_publish_fail fc_begin_publish_fail fc_einval_none_frame fc_desc1_range abandon_top
abandon_suppressed_top abandon_lost_begin abandon_wrong_desc abandon_not_top abandon_stale_token abandon_empty_stack
abandon_refused_admission fault_reason_generic_first fault_reason_hook_first fault_reason_concurrent seal_P1 seal_P2
seal_P3 seal_P4 seal_P5 seal_P6 release_while_inflight'''.split()
CORE_TSAN = ['seal_P1', 'seal_P2', 'seal_P3', 'seal_P4', 'seal_P5', 'seal_P6', 'release_while_inflight',
             'fault_reason_concurrent']
RT_HEAVY = {'cap_262144', 'cap_262145', 'after_cap_counting', 'sat_five'}  # not under TSan (length only)
RT_NEG = {'neg_plain_other': 'rsxcap_other_add', 'neg_late_fields': 'rsxcap_request'}
# THREAD mode with the real drain thread: only in the build with the SC device-memory seam (the TSan build)
RT_THREAD = {'thread_capture_basic', 'thread_capture_shutdown', 'thread_other_stress'}
CT = ['formatter_records', 'draw_kinds', 'lengths', 'capacity_end_494', 'capacity_end_482', 'capacity_begin_121',
      'capacity_ckpt_99', 'pool_unique']
# the core seal prefix (R85): the expected events of the domain after the earlier event 0:1
SEAL_PREFIX = {1: [], 2: [], 3: [], 4: ['1:0'], 5: ['1:0'], 6: ['1:0', '2:1']}

only = set(a.only.split(',')) if a.only else None


def want(g):
    return only is None or g in only


# ---------------------------------------------------------------- builds
exes = {}
for kind in ('core', 'rt', 'ct'):
    for san in (('plain', 'asan', 'tsan') if kind != 'ct' else ('plain',)):
        e = build(out / f'build-{kind}-{san}', kind, san)
        row('build', f'{kind}-{san}', e is not None)
        exes[(kind, san)] = e
rt_list = []
if exes[('rt', 'plain')]:
    r = run([exes[('rt', 'plain')], '--list'], out / 'rt-list.log')
    rt_list = [l for l in r.stdout.split() if not l.startswith('harness_')]

# ---------------------------------------------------------------- core
if want('core'):
    for case in CORE:
        sts = []
        for san in ('plain', 'asan'):
            e = exes[('core', san)]
            sts.append(run_case(e, out / f'core-{san}', 'CORE', case, san)[0] if e else 'INVALID')
        st = 'PASS' if sts == ['PASS', 'PASS'] else ('FAIL' if 'FAIL' in sts else 'INVALID')
        row('core', case, st == 'PASS', f'plain {sts[0]} asan {sts[1]}', status=st)
        if case.startswith('seal_P') and st == 'PASS':
            log = (out / 'core-plain' / f'run-{case}' / 'log.txt').read_text()
            ev = [l for l in log.splitlines() if l.startswith('events')]
            pt = int(case[-1])
            got = ev[0].split()[1:] if ev else None
            exp = ['0:3'] + SEAL_PREFIX[pt]
            row('rd', f'seal_prefix_core_P{pt}', got == exp, f'got {got} want {exp}')

# ---------------------------------------------------------------- rt
cap_dirs = {}
if want('rt'):
    for san in ('plain', 'asan'):
        cap_dirs[san] = out / f'captures-{san}'
        cap_dirs[san].mkdir()
    for case in rt_list:
        if case in RT_NEG or case in RT_THREAD:
            continue
        sts = []
        for san in ('plain', 'asan'):
            e = exes[('rt', san)]
            st, r, cd = run_case(e, out / f'rt-{san}', 'RT', case, san) if e else ('INVALID', None, None)
            sts.append(st)
            if cd:
                for f in cd.iterdir():
                    if f.suffix in ('.prof', '.expect'):
                        shutil.copy(f, cap_dirs[san] / f.name)
        st = 'PASS' if sts == ['PASS', 'PASS'] else ('FAIL' if 'FAIL' in sts else 'INVALID')
        row('rt', case, st == 'PASS', f'plain {sts[0]} asan {sts[1]}', status=st)

# ---------------------------------------------------------------- tsan
if want('tsan'):
    for k in ('core', 'rt'):
        e = exes[(k, 'tsan')]
        if e:
            nm = subprocess.run(['nm', str(e)], capture_output=True, text=True).stdout
            row('tsan', f'no_suppressions_{k}', '__tsan_default_suppressions' not in nm)
    for case in CORE_TSAN:
        e = exes[('core', 'tsan')]
        st = run_case(e, out / 'core-tsan', 'CORE', case, 'tsan')[0] if e else 'INVALID'
        row('tsan', f'core.{case}', st == 'PASS', status=st)
    cap_dirs['tsan'] = out / 'captures-tsan'
    cap_dirs['tsan'].mkdir()
    for case in rt_list:
        if case in RT_HEAVY or case in RT_NEG:
            continue
        e = exes[('rt', 'tsan')]
        st, r, cd = run_case(e, out / 'rt-tsan', 'RT', case, 'tsan', timeout=1800) if e else ('INVALID', None, None)
        if cd:
            for f in cd.iterdir():
                if f.suffix in ('.prof', '.expect'):
                    shutil.copy(f, cap_dirs['tsan'] / f.name)
        row('tsan', case, st == 'PASS', status=st)
    for case, fn in RT_NEG.items():
        e = exes[('rt', 'tsan')]
        st, r, cd = run_case(e, out / 'rt-tsan', 'RT', case, 'tsan', timeout=1800) if e else ('INVALID', None, None)
        n = r.stderr.count('WARNING: ThreadSanitizer: data race') if r else 0
        named = fn in (r.stderr if r else '')
        row('tsan', case, n > 0 and named, f'(NEGATIVE) {n} data-race reports, {fn} named {named}')

# ---------------------------------------------------------------- ct
if want('ct'):
    h = hashlib.sha256(FIXTURE.read_bytes()).hexdigest()
    row('ct', 'fixture_pin', h == FIXTURE_SHA, h[:16])
    e = exes[('ct', 'plain')]
    for case in CT:
        st = run_case(e, out / 'ct', 'CT', case, 'plain', extra=(FIXTURE, out / 'ct' / f'run-{case}'))[0] if e else \
            'INVALID'
        row('ct', case, st == 'PASS', status=st)
    # the pool layout, measured from the object (nm -S of a no-hooks rsxcap.o)
    d = out / 'pool-layout'
    d.mkdir()
    inc = d / 'inc'
    (inc / 'ps3dk').mkdir(parents=True)
    shutil.copy(root / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
    obj = d / 'rsxcap.o'
    r = run([cc, '-std=c11', '-O2', '-c', '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_WITH_SPU', f'-I{inc}',
             f'-I{lib}/core', f'-I{lib}/pal', f'-I{lib}/spu', f'-I{lib}/rsx', lib / 'rsx/rsxcap.c', '-o', obj],
            d / 'build.log')
    sizes = {}
    if r and r.returncode == 0:
        for l in subprocess.run(['nm', '-S', str(obj)], capture_output=True, text=True).stdout.splitlines():
            p = l.split()
            if len(p) == 4 and p[3].startswith('rsxcap_tags_'):
                sizes[p[3]] = int(p[1], 16)
    tot = sum(sizes.values())
    row('ct', 'pool_layout', sizes == {'rsxcap_tags_begin': 2048, 'rsxcap_tags_end': 7936,
                                       'rsxcap_tags_ckpt': 114688} and tot == 124672, f'{sizes} total {tot}')

# ---------------------------------------------------------------- rd
T = a.rlib.parent
checker = out / 'checker' / 'rsxcap_check'
checker.parent.mkdir()
r = run(['rustc', '--edition=2021', '-O', here / 'rsxcap_check.rs', '--extern', f'ps3dk_prof={a.rlib}', '-L',
         f'dependency={T / "deps"}', '-o', checker], out / 'checker' / 'build.log')
have_checker = r is not None and r.returncode == 0
row('rd', 'checker_build', have_checker)
if want('rd') and have_checker:
    for san, cdir in sorted(cap_dirs.items()):
        bad, n = [], 0
        for e in sorted(cdir.glob('*.expect')):
            n += 1
            w = [l.split()[1] for l in e.read_text().splitlines() if l.startswith('exit ')]
            p = e.with_suffix('.prof')
            rr = subprocess.run([str(a.reader), 'check', str(p)], capture_output=True, text=True) if p.exists() else None
            if len(w) != 1 or rr is None or str(rr.returncode) != w[0]:
                bad.append(f'{e.stem}: rc {rr.returncode if rr else "missing"} want {w}')
        stray = [p.name for p in cdir.glob('*.prof') if not p.with_suffix('.expect').exists()]
        row('rd', f'exit_codes_{san}', n > 0 and not bad and not stray, f'{n} captures; {bad[:4]} stray {stray[:4]}')
        r = run([checker, cdir], out / f'checker-{san}.log', timeout=1800)
        txt = r.stdout if r else ''
        row('rd', f'checker_states_{san}', r is not None and r.returncode == 0,
            (txt.strip().splitlines() or [''])[-1])
        row('rd', f'frame_token_{san}', r is not None and 'FrameToken' not in txt)
        row('rd', f'identity_{san}', r is not None and 'IdentityMismatch' not in txt)

    # damage controls on copies of plain captures (framed rewrite, CRC-correct)
    def frames(b):
        o, fr = 16, []
        while o < len(b):
            t, ln, seq = struct.unpack_from('<IIQ', b, o)
            fr.append([t, seq, bytearray(b[o + 16:o + 16 + ln])])
            o += 16 + ln + 4
        return b[:16], fr

    def unframe(head, fr):
        o = bytearray(head)
        for t, seq, p in fr:
            h = struct.pack('<IIQ', t, len(p), seq)
            o += h + p + struct.pack('<I', zlib.crc32(h + bytes(p)))
        return bytes(o)

    def damage(b, old, new, count, end_only=False):
        """old -> new (same length) in every payload; end_only: only inside an END tag (the last occurrence of
        old before each 'coverage=' token)"""
        head, fr = frames(b)
        hits = 0
        for f in fr:
            p = bytes(f[2])
            if end_only:
                q = bytearray(p)
                at = 0
                while True:
                    c = p.find(b'coverage=librsx-draws-v1', at)
                    if c < 0:
                        break
                    w = p.rfind(old, 0, c)
                    if w >= 0 and p.rfind(b'ps3dk.rsxcap.v1 ', 0, c) < w:
                        q[w:w + len(old)] = new
                        hits += 1
                    at = c + 1
                f[2] = q
            else:
                n = p.count(old)
                if n:
                    f[2] = bytearray(p.replace(old, new))
                    hits += n
        return unframe(head, fr), hits

    dd = out / 'damage'
    dd.mkdir()
    src = cap_dirs.get('plain')
    if src:
        cases = [('control', 'rsxcap-close_done', None, None, 0, None),
                 ('end_removed', 'rsxcap-close_done', b'coverage=librsx-draws-v1', b'coverage=librsx-draws-v2', 1,
                  'missing END'),
                 ('tag_digit', 'rsxcap-close_done', b' wire=1 dom=1026 owner=', b' wire=2 dom=1026 owner=', 1,
                  'IdentityMismatch'),  # the END tag only
                 ('duplicate_cap', 'rsxcap-two_conn_wire1', b'ps3dk.rsxcap.v1 cap=2 ', b'ps3dk.rsxcap.v1 cap=1 ', 2,
                  'DuplicateCap')]
        for name, base, old, new, cnt, reason in cases:
            cd = dd / name
            cd.mkdir()
            b = (src / f'{base}.prof').read_bytes()
            hits = 0
            if old:
                b2, hits = damage(b, old, new, cnt, end_only=name == 'tag_digit')
            else:
                b2 = unframe(*frames(b))
            (cd / f'{base}.prof').write_bytes(b2)
            shutil.copy(src / f'{base}.expect', cd / f'{base}.expect')
            r = run([checker, cd], cd / 'checker.log')
            txt = r.stdout if r else ''
            if old is None:
                row('rd', f'damage_{name}', b2 == b and r is not None and r.returncode == 0, 'undamaged copy passes')
            else:
                fl = [l for l in txt.splitlines() if 'FAIL' in l]
                hit = [l for l in fl if reason in l]
                row('rd', f'damage_{name}', hits == cnt and r is not None and r.returncode == 1 and bool(hit),
                    f'{hits} rewrites; {hit[:1]}')

# ---------------------------------------------------------------- mutants
# (name, file, [(old, new)], replace_all, harness kind, case, judge: 'case' or 'checker')
G, PR, RX = 'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/rsxcap.c'
MUT = [
    ('wrapper_rc_lost', PR, [('int prg_mark(prg_dom *d, uint32_t desc) { return prg_mark_r(d, desc, NULL); }',
                              'int prg_mark(prg_dom *d, uint32_t desc)\n{\n    prg_receipt r;\n    int rc = prg_mark_r(d, desc, &r);\n'
                              '    return r.kind == PRG_R_LOST ? PRG_EFAULT : rc;\n}')], 0, 'core', 'wrapper_rc', 'case'),
    ('fc_double_leave', PR, [('    PRG_STEP(d, PRG_SP_P6);\n    leave(d);\n}', '    PRG_STEP(d, PRG_SP_P6);\n    leave(d);\n    leave(d);\n}')],
     0, 'core', 'fc_admission_accounting', 'case'),
    ('fc_no_leave_on_fault', PR, [('            out->rc = PRG_EFAULT; /* nothing committed; the END receipt stands as recorded */\n'
                                   '            leave(d);\n', '            out->rc = PRG_EFAULT; /* nothing committed; the END receipt stands as recorded */\n')],
     0, 'core', 'fc_admission_accounting', 'case'),
    ('leave_after_refused', PR, [('        out->rc = PRG_ESTOPPED; /* nothing submitted, nothing committed, no leave (R7b) */\n',
                                  '        out->rc = PRG_ESTOPPED; /* nothing submitted, nothing committed, no leave (R7b) */\n        leave(d);\n')],
     0, 'core', 'fc_admission_accounting', 'case'),
    ('fc_end_after_commit', PR, [('    PRG_STEP(d, PRG_SP_P2);\n    if (end_desc1) {',
                                  '    PRG_STEP(d, PRG_SP_P2);\n    d->p_frame_owner = owner;\n    d->p_frame_id = frame_id;\n'
                                  '    d->p_owner_ref = owner_ref;\n    if (end_desc1) {')], 0, 'core', 'fc_order', 'case'),
    ('fc_begin_before_commit', PR, [('    PRG_STEP(d, PRG_SP_P4);\n    d->p_frame_owner = owner; /* the commit */',
                                     '    PRG_STEP(d, PRG_SP_P4);\n    if (begin_desc1) {\n        (void)mark_admitted(d, begin_desc1 - 1u, &out->begin);\n'
                                     '        (void)publish(d);\n        begin_desc1 = 0;\n    }\n    d->p_frame_owner = owner; /* the commit */')],
     0, 'rt', 'arm_basic', 'checker'),
    ('fc_always', RX, [('    if (e == 0 || e > RSXCAP_CAPS)\n        return RSXCAP_FRAME_PLAIN;\n    if (RSXCAP_STATE(w) == RSXCAP_REQUESTED) {',
                        '    if (e == 0 || e > RSXCAP_CAPS || RSXCAP_STATE(w) == RSXCAP_TERMINAL) {\n        prg_fc_out o_;\n'
                        '        prg_frame_capture(&g->dom, owner, frame, ref, 0u, 0u, &o_);\n        *committed = o_.committed;\n'
                        '        return o_.rc;\n    }\n    if (RSXCAP_STATE(w) == RSXCAP_REQUESTED) {')], 0, 'rt', 'plain_path_identity', 'case'),
    ('claim_before_prepare', G, [('    if (tok.owner) {\n        const struct slot *s = NULL;',
                                  '    {\n        const rsxcap_ops *ro_ = LD_ACQ(&p->rsxcap);\n        uint32_t cm_ = 0;\n'
                                  '        if (ro_ && ro_->frame(p, g, tok.owner, tok.frame, 0, &cm_) != RSXCAP_FRAME_PLAIN)\n'
                                  '            return done(g, PS3DK_PROF_OK);\n    }\n    if (tok.owner) {\n        const struct slot *s = NULL;')],
     0, 'rt', 'prepare_fail_F5_requested', 'case'),
    ('armed_pub_drops_flags', RX, [('RSXCAP_WORD(RSXCAP_ARMED, w & RSXCAP_FLAGS, e, 0, 0)', 'RSXCAP_WORD(RSXCAP_ARMED, 0, e, 0, 0)')],
     0, 'rt', 'cancel_during_arming', 'case'),
    ('stop_ignored_at_pub', RX, [('        if (w & RSXCAP_F_STOP) { /* STOP at the publication: no ARMED (R21) */',
                                  '        if (0) { /* STOP at the publication: no ARMED (R21) */')], 0, 'rt', 'stop_before_armed_pub', 'case'),
    ('end_lock_fault_ignored', RX, [('    if (rr == -2) { /* R25: a lock fault is terminal */', '    if (0) { /* R25: a lock fault is terminal */')],
     0, 'rt', 'end_lock_fault', 'case'),
    ('cas_state_only', RX, [('        if (S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_ARMING, w & RSXCAP_FLAGS, e, 0, 0)))',
                             '        if ((w = S_LD(SLOT), RSXCAP_STATE(w) == RSXCAP_REQUESTED) &&\n'
                             '            S_CAS(SLOT, &w, RSXCAP_WORD(RSXCAP_ARMING, w & RSXCAP_FLAGS, e, 0, 0)))')],
     0, 'rt', 'stale_owner_after_reclaim', 'case'),
    ('cap_reused', RX, [('RSXCAP_WORD(RSXCAP_CLAIMING, w & RSXCAP_F_STOP, e + 1u, 0, 0)', 'RSXCAP_WORD(RSXCAP_CLAIMING, w & RSXCAP_F_STOP, e ? e : 1u, 0, 0)'),
                        ('    e = e + 1u;\n    rec = &g_rec[e - 1];', '    e = e ? e : 1u;\n    rec = &g_rec[e - 1];')],
     0, 'rt', 'two_conn_wire1', 'case'),
    ('refusal_assigns_cap', RX, [('    if (why != RSXCAP_R_NONE) {\n        (void)p_ext_unlock_site',
                                  '    if (why != RSXCAP_R_NONE) {\n        *cap = e + 1u;\n        (void)p_ext_unlock_site')],
     0, 'rt', 'slot_req_refusals', 'case'),
    ('owner_none_arms', RX, [('        if (g_rec[e - 1].dom != g->dom.domain_id || owner == 0)', '        if (g_rec[e - 1].dom != g->dom.domain_id)')],
     0, 'rt', 'arm_none_token', 'case'),
    ('detach_emits_end', RX, [('            owner_settle(e, RSXCAP_O_ABANDONED, (w & RSXCAP_F_STOP) ? RSXCAP_R_STOPPING : RSXCAP_R_DETACH);',
                               '            {\n                uint32_t cm_;\n                (void)close_(p, g, e, g->dom.p_frame_owner, g->dom.p_frame_id, '
                               'g->dom.p_owner_ref, &cm_);\n            }')], 0, 'rt', 'detach_armed', 'case'),
    ('other_masked_cas', RX, [('        if (__atomic_compare_exchange_n(&rec->other, &w, nw, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))\n            return;',
                               '        __atomic_store_n(&rec->other, nw & 0xffffu, __ATOMIC_RELEASE);\n        return;')],
     0, 'rt', 'other_thread_stale_cas', 'case'),
    ('no_freeze', RX, [('    uint32_t ot = __atomic_fetch_or(&rec->other, RSXCAP_OT_FROZEN, __ATOMIC_ACQ_REL);',
                        '    uint32_t ot = __atomic_load_n(&rec->other, __ATOMIC_ACQUIRE);')], 0, 'rt', 'other_thread_freeze_race_b', 'case'),
    ('capped_off_by_one', RX, [('    if (W.hooks >= RSXCAP_DRAW_CAP) {', '    if (W.hooks >= RSXCAP_DRAW_CAP - 1u) {')], 0, 'rt', 'cap_262144', 'case'),
    ('ckpt_counts_cumulative', RX, [('    memset(&W.blk, 0, sizeof W.blk);\n    if (rc < 0 || pool_register', '    if (rc < 0 || pool_register')],
     0, 'rt', 'block_4097', 'case'),
    ('pool_same_line', RX, [('        d->line = 1u + i;', '        d->line = 1u;')], 0, 'ct', 'pool_unique', 'case'),
    ('abandon_submits_end', RX, [('    (void)prg_zone_abandon(&g->dom, &dr->tok, dr->desc); /* (1) its own admission; nothing submitted */',
                                  '    (void)prg_end_r(&g->dom, &dr->tok, dr->desc, NULL);')], 0, 'rt', 'fault_settles_now', 'case'),
    ('fault_cleared_at_frame', RX, [('    *committed = 0;\n    RSTEP(RT_AFTER_PREPARE);',
                                     '    *committed = 0;\n    __atomic_store_n(&g->dom.fault, 0u, __ATOMIC_RELEASE);\n    RSTEP(RT_AFTER_PREPARE);')],
     0, 'rt', 'fault_no_recovery', 'case'),
    ('claim_publication_store', RX, [('        if (S_CAS(SLOT, &w, nw))\n            break;\n        nw = 0;',
                                      '        __atomic_store_n(SLOT, nw, __ATOMIC_SEQ_CST);\n        break;')], 0, 'rt', 'stop_at_claim_publication', 'case'),
    ('no_post_publication_check', RX, [('    RSTEP(AG_BEFORE_CHECK);\n    {', '    RSTEP(AG_BEFORE_CHECK);\n    if (0) {')],
     0, 'rt', 'detach_during_claiming', 'case'),
    ('fault_settle_deferred', RX, [('        owner_settle(dr->cap, RSXCAP_O_ABANDONED, fault_reason_of(g)); /* (3) settled in this call (R62) */',
                                    '        (void)0;')], 0, 'rt', 'fault_settles_now', 'case'),
    ('closing_ignores_stop', RX, [('        if (w & RSXCAP_F_STOP) { /* R83: STOP in the word this attempt loaded: no normal close, no END */',
                                   '        if (0) { /* R83 */')], 0, 'rt', 'stop_cut_before_cas', 'case'),
    ('cancel_blocks_claim', RX, [('            if ((w & RSXCAP_F_STOP) || RSXCAP_STATE(w) != RSXCAP_IDLE || RSXCAP_EPOCH(w) != e)',
                                  '            if ((w & RSXCAP_FLAGS) || RSXCAP_STATE(w) != RSXCAP_IDLE || RSXCAP_EPOCH(w) != e)')],
     0, 'rt', 'cancel_armed_recycle_next', 'case'),
    ('claim_clears_stop', RX, [('            if ((w & RSXCAP_F_STOP) || RSXCAP_STATE(w) != RSXCAP_IDLE || RSXCAP_EPOCH(w) != e)',
                                '            if (RSXCAP_STATE(w) != RSXCAP_IDLE || RSXCAP_EPOCH(w) != e)'),
                               ('RSXCAP_WORD(RSXCAP_CLAIMING, w & RSXCAP_F_STOP, e + 1u, 0, 0)', 'RSXCAP_WORD(RSXCAP_CLAIMING, 0, e + 1u, 0, 0)')],
     0, 'rt', 'stop_vs_new_epoch_cas_a', 'case'),
    ('no_stop_service', RX, [('        if (RSXCAP_STATE(w) == RSXCAP_REQUESTED && (w & RSXCAP_F_STOP)) {\n            /* R89',
                              '        if (0) {\n            /* R89')], 0, 'rt', 'stop_after_revalidation', 'case'),
    ('resolve_without_lock', RX, [('    if (agent_lock(p) != 0)\n        return refuse(reason, RSXCAP_R_STOPPING); /* ext_fault latched by the lock failure; slot unchanged */\n', ''),
                                  ('p_ext_unlock_site(p, U_RSXCAP_REQUEST)', '0')], 1, 'rt', 'domain0_attach_between_scan_and_claim', 'case'),
    ('loser_writes_record', RX, [('    for (n = 0; n < 3; n++) { /* bound: 3 (R34): only flags may change under a REQUESTED word we still match */',
                                  '    rec->bound = 1u;\n    for (n = 0; n < 3; n++) { /* bound: 3 (R34) */')], 0, 'rt', 'stop_then_stale_owner_cas_b2', 'case'),
    ('pin_failure_skipped', RX, [('            if (pin_slot(g) != 0) {\n                incomplete = 1;',
                                  '            if (pin_slot(g) != 0) {\n                continue;')], 0, 'rt', 'request_pin_saturated', 'case'),
    ('hook_ignores_ext_fault', RX, [('    if (hook_stopped(p))\n        return 0; /* R93', '    if (0)\n        return 0; /* R93')],
     0, 'rt', 'ext_fault_open_draw', 'case'),
    ('draw_end_submits_after_stop', RX, [('(w & RSXCAP_F_STOP) ||\n        hook_stopped(g_prof)) {', 'hook_stopped(g_prof)) {')],
     0, 'rt', 'stop_mid_zone', 'case'),
    ('ckpt_lock_fault_as_missing', RX, [('        if (rc >= 0 && LD_RLX(&p->lock_failed)) {', '        if (0) {')],
     0, 'rt', 'ckpt_lock_fault_stops_hook', 'case'),
    ('unlock_fail_reports_cap0', RX, [('    if (unlock_failed)\n        return PS3DK_PROF_ESTOPPED; /* R91: never cap 0, never ACCEPTED */',
                                       '    if (unlock_failed)\n        return *cap = 0, PS3DK_PROF_ESTOPPED;')], 0, 'rt', 'agent_unlock_fail_withdrawable', 'case'),
]
if want('mut'):
    for name, f, edits, rall, kind, case, judge in MUT:
        md = out / f'mut-{name}'
        shutil.copytree(lib, md / 'src')
        p = md / 'src' / f
        s = p.read_text()
        okedit = True
        for old, new in edits:
            n = s.count(old)
            if n == 0 or (n != 1 and not rall):
                row('mut', name, False, f'edit matched {n} times', status='INVALID')
                okedit = False
                break
            s = s.replace(old, new)
        if not okedit:
            continue
        p.write_text(s)
        e = build(md / 'b', kind, 'plain', src=md / 'src')
        if not e:
            row('mut', name, False, 'build failed', status='INVALID')
            continue
        prefix = {'core': 'CORE', 'rt': 'RT', 'ct': 'CT'}[kind]
        extra = (FIXTURE, md / f'run-{case}') if kind == 'ct' else ()
        st, r, cd = run_case(e, md, prefix, case, 'plain', extra=extra)
        if judge == 'case':
            killed = st == 'FAIL'
            fl = [l for l in (r.stdout if r else '').splitlines() if 'FAIL' in l][:1]
            row('mut', name, killed, f'{st} by {case}: {fl}', status='PASS' if killed else ('INVALID' if st == 'INVALID' else 'FAIL'))
        else:
            if st != 'PASS':
                row('mut', name, False, f'{case} {st} (want PASS, then the checker)', status='INVALID' if st == 'INVALID' else 'FAIL')
                continue
            cdir = md / 'cap'
            cdir.mkdir()
            for x in cd.iterdir():
                if x.suffix in ('.prof', '.expect'):
                    shutil.copy(x, cdir / x.name)
            r2 = run([checker, cdir], md / 'checker.log') if have_checker else None
            fl = [l for l in (r2.stdout if r2 else '').splitlines() if 'FAIL' in l][:2]
            row('mut', name, r2 is not None and r2.returncode == 1 and bool(fl), f'checker on {case}: {fl}')

# ---------------------------------------------------------------- reg
if want('reg'):
    # harness controls: a stalled case and a crashing case are INVALID, never PASS
    e = exes[('core', 'plain')]
    st1 = run_case(e, out / 'controls', 'CORE', 'harness_stall', 'plain', timeout=600)[0] if e else '?'
    e2 = exes[('rt', 'plain')]
    st2 = run_case(e2, out / 'controls', 'RT', 'harness_crash', 'plain')[0] if e2 else '?'
    st3 = run_case(e2, out / 'controls', 'RT', 'harness_stall', 'plain', timeout=600)[0] if e2 else '?'
    row('reg', 'harness_controls', st1 == 'INVALID' and st2 == 'INVALID' and st3 == 'INVALID',
        f'core stall {st1}, rt crash {st2}, rt stall {st3}')
    # input pins
    r15 = root.parents[1] / 'docs/local/prof-viewer/rsx-frame-capture-design-r15-amendment.md'
    if not r15.exists():
        r15 = Path('/mnt/c/Users/FirebirdTA01/source/repos/PS3_Custom_Toolchain/docs/local/prof-viewer/'
                   'rsx-frame-capture-design-r15-amendment.md')
    h15 = hashlib.sha256(r15.read_bytes()).hexdigest()[:16] if r15.exists() else 'missing'
    hrd = hashlib.sha256(READER_SRC.read_bytes()).hexdigest()[:16]
    (out / 'paths.txt').write_text(
        f'worktree {root}\nfixture {FIXTURE} {hashlib.sha256(FIXTURE.read_bytes()).hexdigest()}\n'
        f'reader source {READER_SRC} {hrd}\nr15 historical PASS hash 4a68220edd43f566\n'
        f'r15 adopted file c36f615a6321455a (on disk {h15})\nreader {a.reader}\nrlib {a.rlib}\n')
    row('reg', 'input_pins', h15 == 'c36f615a6321455a' and hrd == READER_SHA16, f'r15 {h15} reader {hrd}')
    # the diff-based hygiene rows
    # the diff comes from a file made where git can read the worktree; missing or empty fails closed
    diff = a.diff.read_text() if a.diff and a.diff.exists() else ''
    row('reg', 'diff_present', 'diff --git a/sdk/libps3dkprof/src/rsx/prg.c' in diff, f'{len(diff)} bytes from {a.diff}')
    added = [l[1:] for l in diff.splitlines() if l.startswith('+') and not l.startswith('+++')]
    added += (lib / 'rsx/rsxcap.c').read_text().splitlines() + (lib / 'rsx/rsxcap.h').read_text().splitlines()
    (out / 'checks').mkdir()
    bad = [l for l in added if re.search(r'\b(now_us|mftb|pal_sleep_us|usleep|nanosleep|alarm|timeout_us|clock_gettime|'
                                         r'pal_mono_now_us|p_mono_now|time\s*\()', l)]
    (out / 'checks/no-timers.txt').write_text('\n'.join(bad) + '\n')
    row('reg', 'no_timers', not bad, f'{len(bad)} lines')
    lb = []
    for fn in ('rsx/rsxcap.c',):
        ls = (lib / fn).read_text().splitlines()
        for i, l in enumerate(ls):
            # a do { ... } while (0) statement macro (its line continues with a backslash) is not a loop
            if re.search(r'\b(for|while)\s*\(|\bdo\s*\{', l) and not l.strip().startswith(('/*', '*')) and \
                    not re.match(r'\s*\}\s*while', l) and not l.rstrip().endswith('\\'):
                if 'bound:' not in l and 'bound:' not in (ls[i - 1] if i else ''):
                    lb.append(f'{fn}:{i + 1}: {l.strip()}')
    for l in [x for x in diff.split('\ndiff --git') if 'rsx/prg.c' in x.split('\n')[0] or 'rsx/gpu_runtime.c' in x.split('\n')[0]]:
        for x in l.splitlines():
            if x.startswith('+') and re.search(r'\b(for|while)\s*\(|\bdo\s*\{', x) and 'bound:' not in x:
                lb.append(f'diff: {x}')
    (out / 'checks/loop-bounds.txt').write_text('\n'.join(lb) + '\n')
    row('reg', 'loop_bounds', not lb, f'{lb[:3]}')
    # no heap in rsxcap.o; no test seam in the archive-shaped objects (host stand-in for the PPU ext archive)
    d = out / 'archive'
    d.mkdir()
    inc = d / 'inc'
    (inc / 'ps3dk').mkdir(parents=True)
    shutil.copy(root / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
    objs, okb = [], True
    ARCH_SRCS = ('core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c', 'spu/ext_runtime.c', 'spu/psp_ppu.c',
                 'spu/psp_elf.c', 'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/gpu_port_gcm.c', 'rsx/rsxcap.c')
    for s in ARCH_SRCS:
        o = d / (Path(s).stem + '.o')
        r = run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-c', '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST',
                 '-DPS3DK_PROF_WITH_SPU', f'-I{inc}', f'-I{lib}/core', f'-I{lib}/pal', f'-I{lib}/spu', f'-I{lib}/rsx',
                 lib / s, '-o', o], d / f'{o.stem}.log')
        okb &= r is not None and r.returncode == 0
        objs.append(o)
    def judge_archive(archive, obj, tag):
        """(archive ok, alloc ok, info): both require ar/nm to succeed and real member and symbol evidence; a tool
        failure or empty output can never pass"""
        ra = run(['ar', 't', archive], d / f'ar-t-{tag}.log')
        rn = run(['nm', archive], d / f'nm-{tag}.log')
        ro = run(['nm', obj], d / f'nm-obj-{tag}.log')
        if ra is None or rn is None or ro is None or ra.returncode or rn.returncode or ro.returncode:
            return False, False, f'tool failed (ar {ra.returncode if ra else "timeout"}, nm {rn.returncode if rn else "timeout"}, nm obj {ro.returncode if ro else "timeout"})'
        members = set(ra.stdout.split())
        want_members = {f'{Path(s).stem}.o' for s in ARCH_SRCS}
        nm_a, nm_o = rn.stdout, ro.stdout
        defined = set(re.findall(r'^[0-9a-f]+ [TDBR] (\S+)$', nm_a, re.M))
        want_syms = {'ps3dk_prof__rsxcap_request', 'ps3dk_prof__rsxcap_draw_begin', 'ps3dk_prof__rsxcap_enable',
                     'ps3dk_prof__rsxcap_slot', 'ps3dk_prof__prg_frame_capture', 'ps3dk_prof__prg_zone_abandon',
                     'ps3dk_prof_gpu_frame'}
        seams = [l for l in nm_a.splitlines() if re.search(r'ps3dk_prof_test_|prg_test_|ps3dk_prof__test_', l)]
        undef_o = set(re.findall(r'^\s+U (\S+)$', nm_o, re.M))
        def_o = set(re.findall(r'^[0-9a-f]+ [TDBRbdr] (\S+)$', nm_o, re.M))
        heap = sorted(undef_o & {'malloc', 'calloc', 'realloc', 'free', 'ps3dk_prof__pal_alloc', 'posix_memalign'})
        arch_ok = members == want_members and want_syms <= defined and not seams
        alloc_ok = 'ps3dk_prof__rsxcap_request' in def_o and 'rsxcap_tags_end' in def_o and \
            'ps3dk_prof__gpu_desc' in undef_o and not heap
        return arch_ok, alloc_ok, (f'{len(members)} members (want {len(want_members)}), '
                                   f'{len(want_syms & defined)}/{len(want_syms)} expected symbols, {len(seams)} seams, '
                                   f'heap {heap}, {len(undef_o)} undefined in rsxcap.o')
    if okb:
        ra = run(['ar', 'rcs', d / 'libps3dkprof_ext-host.a', *objs], d / 'ar.log')
        aok, lok, info = judge_archive(d / 'libps3dkprof_ext-host.a', d / 'rsxcap.o', 'real')
        if ra is None or ra.returncode:
            aok = lok = False
            info = 'ar rcs failed; ' + info
        (out / 'checks/archive.txt').write_text(info + '\n')
        row('reg', 'archive_symbols', aok, info + ' (host-built stand-in)')
        row('reg', 'no_alloc', lok, info)
        # the failed-tool NEGATIVE control: a missing archive / object can never pass either judge
        naok, nlok, ninfo = judge_archive(d / 'no-such-archive.a', d / 'no-such.o', 'neg')
        row('reg', 'archive_tool_failure_control', not naok and not nlok, f'(NEGATIVE) {ninfo}')
    else:
        row('reg', 'archive_symbols', False, 'build failed')
        row('reg', 'no_alloc', False, 'build failed')
    # shipped members unchanged (the Makefile's CORE / LIVE / AUTO lists)
    row('reg', 'shipped_members', bool(diff) and 'diff --git a/sdk/libps3dkprof/Makefile' not in diff,
        'the Makefile (CORE / LIVE / AUTO member lists) is not in the diff')
    # the ext member list and its checker contract (self-test of the checker; no target build here)
    mk = (root / 'sdk/libps3dkprof/ext-members.mk').read_text()
    r = run([sys.executable, '-B', root / 'tests/host/ps3dk-prof-ext-target/r4b_selftest.py'], out / 'r4b-selftest.log')
    row('reg', 'ext_target', 'gpu_port_gcm rsxcap\n' in mk and r is not None and r.returncode == 0,
        'member rsxcap + checker self-test')
    if not a.skip_regress:
        rg = out / 'regress'
        rg.mkdir()
        r = run([sys.executable, '-B', root / 'tests/host/ps3dk-prof-gpu-int/r2-test.py', rg / 'r2', '--reader',
                 a.reader], rg / 'r2-test.log', timeout=7200)
        row('reg', 'r2', r is not None and r.returncode == 0, (r.stdout.strip().splitlines() or [''])[-1] if r else
            'timeout')
        r = run([sys.executable, '-B', root / 'tests/host/ps3dk-prof-gpu-int/r4a-test.py', rg / 'r4a', '--reader',
                 a.reader], rg / 'r4a-test.log', timeout=7200)
        row('reg', 'r4a', r is not None and r.returncode == 0, (r.stdout.strip().splitlines() or [''])[-1] if r else
            'timeout')
        env = dict(os.environ)
        r = run(['cargo', 'test', '--release', '-p', 'ps3dk-prof', '--test', 'rsx_capture'], rg / 'cargo-rsx.log',
                timeout=3600, cwd=root / 'tools', env=env)
        row('reg', 'cargo_rsx', r is not None and r.returncode == 0, (r.stdout.strip().splitlines() or [''])[-1] if r
            else 'timeout')
    tree = a.tree_scan.read_text() if a.tree_scan and a.tree_scan.exists() else ''
    new_files = sorted(p for p in here.iterdir() if p.is_file()) + [lib / 'rsx/rsxcap.c', lib / 'rsx/rsxcap.h']
    blob = '\n'.join(p.read_text(errors='replace') for p in new_files) + '\n' + '\n'.join(str(p.relative_to(root)) for p in new_files)
    r2 = subprocess.run([sys.executable, '-B', str(root / 'scripts/neutral-wording-scan.py'), '--stdin'], input=blob,
                        capture_output=True, text=True, cwd=root)
    (out / 'checks/no-names.txt').write_text(tree + '\n--- new files (stdin)\n' + r2.stdout + r2.stderr)
    row('reg', 'no_names', 'neutral-wording: PASS (' in tree and 'tracked text files' in tree and r2.returncode == 0,
        'the --tree scan (run where git works) + the new untracked files (stdin)')

# ---------------------------------------------------------------- the result
groups = {}
for g, n, st in rows:
    groups.setdefault(g, [0, 0, 0])
    groups[g][{'PASS': 0, 'FAIL': 1, 'INVALID': 2}[st]] += 1
lines = [f'{g}: {v[0]} pass, {v[1]} fail, {v[2]} invalid' for g, v in sorted(groups.items())]
bad = [f'{g}.{n} {st}' for g, n, st in rows if st != 'PASS']
res = ('PASS' if not bad else 'FAIL') + f' {len(rows) - len(bad)}/{len(rows)} rows\n' + '\n'.join(lines) + '\n' + \
      ('not passing:\n  ' + '\n  '.join(bad) + '\n' if bad else '')
(out / 'RESULT').write_text(res)
print('S2 HOST ' + res)
# the finalizer: RESULT first, then INDEX.sha256 of every other file (sorted by path), written last; verified by an
# exact set compare of the listed paths against the directory, then nothing more is written inside
listed = sorted(str(p.relative_to(out)) for p in out.rglob('*') if p.is_file() and p.name != 'INDEX.sha256')
idx = ''.join(f'{hashlib.sha256((out / p).read_bytes()).hexdigest()}  {p}\n' for p in listed)
(out / 'INDEX.sha256').write_text(idx)
now = sorted(str(p.relative_to(out)) for p in out.rglob('*') if p.is_file() and p.name != 'INDEX.sha256')
if now != listed:
    print('S2 HOST FINALIZER FAIL: the directory changed after INDEX')
    sys.exit(1)
sys.exit(1 if bad else 0)
