#!/usr/bin/env python3
"""P3.3 R4a: the REAL GCM port inside the REAL runtime, HOST ONLY (design 5bb6b922 + r2 delta 7cb3397d, codex GO
18:55; harness corrections codex 19:26).  Host-model evidence, not target evidence: the executor decoding the
command words is ours; Q1/Q2 stay hardware questions; A1/A2 stay assumptions; Mode C's SC device memory and its
completion accounting are EMULATOR properties, stronger than anything real hardware promises.

Rows (exit 0 only if all pass):
  1. Mode S (r4a_int.c without GPP_HOST_DEVMEM: the port's real volatile path; APP mode, one thread), plain and
     non-recovering ASan/UBSan: every case PASS, rc 0, no sanitizer text in stderr;
  2. Mode C (GPP_HOST_DEVMEM: every timer/label access an SC atomic; THREAD mode + an executor thread whose packets
     count as done only after their device writes), plain and TSan: every case PASS, no sanitizer text;
  3. NEGATIVE controls (TSan must REPORT a 'ThreadSanitizer: data race'): the executor storing plainly
     (R4A_PLAIN_EXEC), and the port's seam bypassed for its label load;
  4. the PINNED capture inventory of every run directory: exactly the expected files (log, capture, expect, truth /
     dtruth as pinned per case), none missing, none extra; with deletion/extra-file controls and a clean control;
  5. the committed reader's exact exit code on every pinned capture (per .expect);
  6. the fail-closed checker (ONE numbered domain per capture): suite r4a on Mode S plain, r4a-tsan on Mode C TSan;
  7. the per-domain checker (r4a_domains.py) on every two-domain capture of every build, against its dtruth (each
     domain's ordered events with raw timers from the executor's own log, and its FINAL), with framed, CRC-correct
     controls it must reject: the two domains' ids swapped, and one domain's raw timers duplicated into the other;
  8. mutants (plain builds), each KILLED by its NAMED case: rc 1 and a FAIL line of that case containing the named
     text; a crash, timeout or build failure INVALIDATES the row (never a kill).  One mutant is of the HARNESS: the
     old queue-empty Finish.

usage: r4a-test.py OUT --reader READER"""
import argparse
import os
from pathlib import Path
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
S_CASES = '''s-e2e s-identity s-prepare-fail-first s-prepare-fail-middle s-prepare-fail-last s-prepare-fail-label
    s-prepare-fail-reserved s-prg-attach-fail s-hooks s-release-keeps s-unlock-fail s-fifo-refusal'''.split()
C_CASES = ['c-e2e', 'c-finish-pending', 'c-ack-release', 'c-stale-pin', 'c-seal-in-reserve']
TWO_DOMAIN = {'s-identity', 's-release-keeps', 'c-ack-release', 'c-stale-pin'}  # .dtruth, no .truth
NO_EXPECT = {'s-unlock-fail'}  # a faulted extension: the capture is named, nothing is claimed about it


def pinned(case):
    """the exact file set of a case's run directory"""
    n = f'r4a-{case}'
    files = {'log.txt', f'{n}.prof'}
    if case not in NO_EXPECT:
        files.add(f'{n}.expect')
        files.add(f'{n}.dtruth' if case in TWO_DOMAIN else f'{n}.truth')
    return files


SAN = {'asan': ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'], 'tsan': ['-fsanitize=thread'],
       'plain': []}
SAN_TEXT = ('runtime error', 'Sanitizer', 'ThreadSanitizer', 'LeakSanitizer')


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def run(cmd, log, timeout=180, env=None):
    try:
        r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        Path(log).write_text(' '.join(map(str, cmd)) + '\n\nTIMEOUT\n')
        return None
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + '\n--- stderr\n' + r.stderr)
    return r


def build(d, src, mode_c, san, extra=(), harness=None):
    inc = d / 'inc'
    (inc / 'ps3dk').mkdir(parents=True, exist_ok=True)
    shutil.copy(root / 'sdk/include/ps3dk/prof.h', inc / 'ps3dk/prof.h')
    flags = ['-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror', '-DPS3DK_PROF=1',
             '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DPS3DK_PROF_WITH_SPU', f'-I{inc}', f'-I{src}/core',
             f'-I{src}/pal', f'-I{src}/spu', f'-I{src}/rsx', *SAN[san], *(['-DGPP_HOST_DEVMEM'] if mode_c else []),
             *extra]
    srcs = [src / f for f in ('core/prof_core.c', 'core/prof_drain.c', 'pal/pal_host.c', 'spu/psp_ppu.c',
                              'spu/psp_spu.c', 'spu/ext_runtime.c', 'spu/psp_elf.c', 'spu/psp_host_emul.c',
                              'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/gpu_port_gcm.c')] + [harness or here / 'r4a_int.c']
    exe = d / 'r4a'
    r = run([cc, *flags, *srcs, '-o', exe], d / 'build.log')
    return exe if r is not None and r.returncode == 0 else None


def run_case(exe, d, case, tsan=False):
    """(status, stdout, stderr, dir): PASS, FAIL (rc 1 and a FAIL line, no sanitizer text) or INVALID"""
    cd = d / f'run-{case}'
    cd.mkdir()
    env = dict(os.environ, TSAN_OPTIONS='halt_on_error=0 report_signal_unsafe=0') if tsan else None
    pre = ['setarch', 'x86_64', '-R'] if tsan else []  # TSan needs low ASLR entropy on this kernel (as R2)
    r = run([*pre, exe, case, cd], cd / 'log.txt', timeout=300, env=env)
    if r is None:
        return 'INVALID', '', 'TIMEOUT', cd
    san = any(t in r.stderr for t in SAN_TEXT)
    fails = [l for l in r.stdout.splitlines() if l.startswith(f'R4A {case} FAIL')]
    if r.returncode == 0 and f'R4A {case} PASS' in r.stdout and not san and not fails:
        return 'PASS', r.stdout, r.stderr, cd
    if r.returncode == 1 and fails and not san:
        return 'FAIL', r.stdout, r.stderr, cd
    return 'INVALID', r.stdout, r.stderr, cd


def inventory_problems(dirs):
    """dirs: {case: run dir}.  Every pinned file exactly once, nothing else."""
    bad = []
    for case, cd in sorted(dirs.items()):
        have = {p.name for p in cd.iterdir()} if cd.is_dir() else set()
        want = pinned(case)
        if have - want:
            bad.append(f'{case}: extra {sorted(have - want)}')
        if want - have:
            bad.append(f'{case}: missing {sorted(want - have)}')
    return bad


def reader_rows(dirs, tag):
    bad, n = [], 0
    for case, cd in sorted(dirs.items()):
        if case in NO_EXPECT:
            continue
        e = cd / f'r4a-{case}.expect'
        n += 1
        want = [l.split()[1] for l in e.read_text().splitlines() if l.startswith('exit ')] if e.exists() else []
        r = subprocess.run([str(a.reader), 'check', str(e.with_suffix('.prof'))], capture_output=True, text=True)
        if len(want) != 1 or str(r.returncode) != want[0]:
            bad.append(f'{case}: rc {r.returncode} want {want}')
    row(f'reader exit codes ({tag})', n == sum(c not in NO_EXPECT for c in dirs) and not bad, f'{n} pinned {bad}')


def oracle_row(dirs, suite):
    cap = out / f'cap-{suite}'
    cap.mkdir()
    for case, cd in dirs.items():
        if case in TWO_DOMAIN or case in NO_EXPECT:
            continue
        for ext in ('.prof', '.truth', '.expect'):
            shutil.copy(cd / f'r4a-{case}{ext}', cap / f'r4a-{case}{ext}')
    r = run([sys.executable, '-I', here / 'gpu_oracle.py', cap, '--suite', suite], out / f'oracle-{suite}.log')
    row(f'oracle suite {suite}', r is not None and r.returncode == 0, (r.stdout.strip().splitlines() or [''])[-1]
        if r else 'timeout')


def domains(prof, dtruth, log):
    r = run([sys.executable, '-I', here / 'r4a_domains.py', prof, dtruth], log)
    return (r.returncode if r else None), (r.stdout.strip() if r else 'timeout')


# ---- framed rewriting for the per-domain controls (header, then <type,len,seq> + payload + crc32) ----
def read_frames(b):
    o, fr = 16, []
    while o < len(b):
        t, ln, seq = struct.unpack_from('<IIQ', b, o)
        fr.append([t, seq, bytearray(b[o + 16:o + 16 + ln])])
        o += 16 + ln + 4
    return b[:16], fr


def write_frames(head, fr):
    o = bytearray(head)
    for t, seq, p in fr:
        h = struct.pack('<IIQ', t, len(p), seq)
        o += h + p + struct.pack('<I', zlib.crc32(h + bytes(p)))
    return bytes(o)


def did_off(t):
    return {8: 4, 14: 0, 15: 0, 16: 4, 10: 0}.get(t)


def ctl_swap(b, x, y):
    head, fr = read_frames(b)
    for t, _s, p in fr:
        o = did_off(t)
        if o is not None and len(p) >= o + 4:
            d = struct.unpack_from('<I', p, o)[0]
            if d in (x, y):
                struct.pack_into('<I', p, o, y if d == x else x)
    return write_frames(head, fr)


def ctl_dup_raw(b, src, dst):
    """dst's event raw timers replaced by src's at the same seq (equal-sized streams)"""
    head, fr = read_frames(b)
    raws = {}
    for t, _s, p in fr:
        if t == 14 and struct.unpack_from('<I', p, 0)[0] == src:
            first, n = struct.unpack_from('<QI', p, 8)
            for i in range(n):
                raws[first + i] = struct.unpack_from('<Q', p, 20 + 28 * i)[0]
    hit = 0
    for t, _s, p in fr:
        if t == 14 and struct.unpack_from('<I', p, 0)[0] == dst:
            first, n = struct.unpack_from('<QI', p, 8)
            for i in range(n):
                if first + i in raws:
                    struct.pack_into('<Q', p, 20 + 28 * i, raws[first + i])
                    hit += 1
    return write_frames(head, fr), hit


# ---- 1 + 2: the cases ----
runs = {}
for mode_c, cases in ((False, S_CASES), (True, C_CASES)):
    for san in (('plain', 'tsan') if mode_c else ('plain', 'asan')):
        tag = f'{"C" if mode_c else "S"}-{san}'
        d = out / tag
        d.mkdir()
        exe = build(d, lib, mode_c, san)
        if not exe:
            row(f'build {tag}', False)
            continue
        runs[tag] = {}
        for c in cases:
            st, so, se, cd = run_case(exe, d, c, tsan=san == 'tsan')
            row(f'{tag} {c}', st == 'PASS', st if st != 'PASS' else '')
            runs[tag][c] = cd


# ---- 3: negative controls (TSan must report) ----
def tsan_reports(tag, src, extra):
    d = out / f'neg-{tag}'
    d.mkdir()
    exe = build(d, src, True, 'tsan', extra)
    if not exe:
        row(f'negative control {tag}', False, 'build failed')
        return
    st, so, se, cd = run_case(exe, d, 'c-e2e', tsan=True)
    n = se.count('WARNING: ThreadSanitizer: data race')
    row(f'negative control {tag} (TSan must report)', n > 0, f'{n} data-race reports')


tsan_reports('plain-executor', lib, ['-DR4A_PLAIN_EXEC'])
nb = out / 'src-seam-bypass'
shutil.copytree(lib, nb)
p = nb / 'rsx/gpu_port_gcm.c'
s = p.read_text()
old = '#define GPP_LD32(pc, a) ((pc)->dev->ld32(a))'
if s.count(old) == 1:
    p.write_text(s.replace(old, '#define GPP_LD32(pc, a) ((void)(pc), *(volatile uint32_t *)(a))'))
    tsan_reports('seam-bypassed-label-load', nb, [])
else:
    row('negative control seam-bypassed-label-load', False, 'edit did not match exactly once')

# ---- 4: the pinned inventory, then its controls ----
for tag, dirs in sorted(runs.items()):
    bad = inventory_problems(dirs)
    row(f'inventory {tag}', not bad, '; '.join(bad))
if 'S-plain' in runs and 'C-tsan' in runs:
    def ctl(name, tag, case, act, want_bad):
        cp = out / f'invctl-{name}'
        shutil.copytree(runs[tag][case], cp / case)
        act(cp / case)
        bad = inventory_problems({case: cp / case})
        row(f'inventory control {name}', bool(bad) == want_bad, '; '.join(bad) or 'clean')
    ctl('clean', 'S-plain', 's-identity', lambda d: None, False)
    ctl('delete-two-domain-expect', 'S-plain', 's-identity', lambda d: (d / 'r4a-s-identity.expect').unlink(), True)
    ctl('delete-two-domain-capture', 'C-tsan', 'c-ack-release', lambda d: (d / 'r4a-c-ack-release.prof').unlink(), True)
    ctl('delete-dtruth', 'S-plain', 's-release-keeps', lambda d: (d / 'r4a-s-release-keeps.dtruth').unlink(), True)
    ctl('extra-file', 'C-tsan', 'c-stale-pin', lambda d: (d / 'r4a-c-stale-pin.truth').write_text(''), True)

# ---- 5 + 6: reader and oracle ----
for tag, dirs in sorted(runs.items()):
    reader_rows(dirs, tag)
if 'S-plain' in runs:
    oracle_row(runs['S-plain'], 'r4a')
if 'C-tsan' in runs:
    oracle_row(runs['C-tsan'], 'r4a-tsan')

# ---- 7: the per-domain checker and its framed controls ----
for tag, dirs in sorted(runs.items()):
    for case in sorted(TWO_DOMAIN & set(dirs)):
        cd = dirs[case]
        rc, msg = domains(cd / f'r4a-{case}.prof', cd / f'r4a-{case}.dtruth', out / f'domains-{tag}-{case}.log')
        row(f'domains {tag} {case}', rc == 0, msg.splitlines()[-1] if msg else '')
if 'S-plain' in runs:
    cd = runs['S-plain']['s-identity']
    b = (cd / 'r4a-s-identity.prof').read_bytes()
    dt = cd / 'r4a-s-identity.dtruth'
    cdir = out / 'domctl'
    cdir.mkdir()
    same = write_frames(*read_frames(b)) == b
    row('domains control: re-framing round-trips byte-identically', same)
    (cdir / 'swapped.prof').write_bytes(ctl_swap(b, 1026, 1027))
    rc, msg = domains(cdir / 'swapped.prof', dt, cdir / 'swapped.log')
    row('domains control: domain ids swapped (CRC-correct) -> rejected for differing events', rc == 1 and
        'DOMAINS FAIL domain 1026: events differ at 0' in msg and 'CRC' not in msg and 'dtruth' not in msg.split(
            'events differ')[0], msg.splitlines()[0] if msg else '')
    dup, hit = ctl_dup_raw(b, 1026, 1027)
    (cdir / 'duplicated.prof').write_bytes(dup)
    rc, msg = domains(cdir / 'duplicated.prof', dt, cdir / 'duplicated.log')
    row('domains control: 1026 raw timers duplicated into 1027 (CRC-correct) -> rejected for differing events',
        rc == 1 and hit == 60 and msg.splitlines()[0].startswith('DOMAINS FAIL domain 1027: events differ at 0')
        and 'domain 1026' not in msg, f'{hit} events rewritten; ' + (msg.splitlines()[0] if msg else ''))

# ---- 8: mutants: (name, file, old, new, mode_c, case, named text); file 'HARNESS' = r4a_int.c ----
G, P = 'rsx/gpu_runtime.c', 'rsx/gpu_port_gcm.c'
MUT = [
    ('devport-other-slot-ctx', G, '        gpp_bind(&g->devport, &g->gpp);', '        gpp_bind(&g->devport, &p->gpu[0].gpp);',
     False, 's-identity', 'own domain'),
    ('reprepare-skipped', G, '        if (gpp_prepare(&g->gpp, gdev,', '        if (!g->gpp.ready && gpp_prepare(&g->gpp, gdev,',
     False, 's-release-keeps', 'bank B poisoned'),
    ('release-clears-port', G, '        ST_SC_(&g->id, 0u);                          /* stale callers now miss */',
     '        ST_SC_(&g->id, 0u);                          /* stale callers now miss */\n        gpp_release(&g->gpp);',
     False, 's-release-keeps', 'prepared state kept'),
    ('prepare-failure-ignored', G, ' label) != PRG_OK) {\n            rc = PS3DK_PROF_EINVAL;',
     ' label) != PRG_OK && 0) {\n            rc = PS3DK_PROF_EINVAL;', False, 's-prepare-fail-first', 'attach refused'),
    ('hook-conflict-accepted', G, '    if (dev && gdev)\n        return PS3DK_PROF_EINVAL;', '    if (0)\n        return 0;',
     False, 's-hooks', 'both hooks'),
    ('select-pinned-slot', G, '        if (LD_ACQ(&p->gpu[i].state) == GPU_S_FREE && LD_SC_(&p->gpu[i].users) == 0)',
     '        if (LD_ACQ(&p->gpu[i].state) == GPU_S_FREE)', True, 'c-ack-release', 'not re-selected while pinned'),
    ('select-pinned-slot-foreign', G,
     '        if (LD_ACQ(&p->gpu[i].state) == GPU_S_FREE && LD_SC_(&p->gpu[i].users) == 0)',
     '        if (LD_ACQ(&p->gpu[i].state) == GPU_S_FREE)', True, 'c-stale-pin', 'not re-selected under the foreign pin'),
    ('timestamp-bit28', P, '#define GPP_QUERY_TIMESTAMP (1u << 24)', '#define GPP_QUERY_TIMESTAMP (1u << 28)', False,
     's-e2e', 'every word valid'),
    ('fifo-refusal-ignored', P, '        if (!gpp_ctx_valid(c) || c->callback == NULL || gpp_invoke(c, GPP_PAIR_WORDS) != 0)\n'
     '            return 1;', '        if (!gpp_ctx_valid(c) || c->callback == NULL || gpp_invoke(c, GPP_PAIR_WORDS) != 0)\n'
     '            return 0;', False, 's-fifo-refusal', 'FIFO losses'),
    ('harness-queue-empty-finish', 'HARNESS',
     '    pthread_mutex_lock(&g_qm);\n    uint32_t submitted = g_qh;\n    pthread_mutex_unlock(&g_qm);\n'
     '    return atomic_load(&g_exec_done) == submitted;',
     '    pthread_mutex_lock(&g_qm);\n    int empty = g_qt == g_qh;\n    pthread_mutex_unlock(&g_qm);\n    return empty;',
     True, 'c-finish-pending', 'Finish is pending'),
]
for name, f, old, new, mode_c, case, text in MUT:
    md = out / f'mut-{name}'
    shutil.copytree(lib, md / 'src')
    harness = None
    if f == 'HARNESS':
        harness = md / 'r4a_int.c'
        shutil.copy(here / 'r4a_int.c', harness)
        p = harness
    else:
        p = md / 'src' / f
    s = p.read_text()
    if s.count(old) != 1:
        row(f'mutant {name}', False, f'edit matched {s.count(old)} times')
        continue
    p.write_text(s.replace(old, new))
    exe = build(md, md / 'src', mode_c, 'plain', harness=harness)
    if not exe:
        row(f'mutant {name}', False, 'INVALID: build failed')
        continue
    st, so, se, cd = run_case(exe, md, case)
    hit = [l for l in so.splitlines() if l.startswith(f'R4A {case} FAIL') and text in l]
    row(f'mutant {name}', st == 'FAIL' and bool(hit), f'{st} by {case}: {hit[:1]}')

bad = [n for n, ok in rows if not ok]
print(f'R4A HOST {"PASS" if not bad else "FAIL"}: {len(rows)} rows, {len(bad)} failed {bad if bad else ""}'.rstrip())
sys.exit(1 if bad else 0)
