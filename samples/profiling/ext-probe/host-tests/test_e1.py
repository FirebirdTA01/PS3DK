#!/usr/bin/env python3
"""E1 host tests (design p3.3 E1 r3 section F + r4 section 4 + review 05:30 and 10:25): the PPU flow of source/main.c
built for the host (e1_host.h fakes, the real runtime, the real SPU core and writer on a host thread, the R4a GCM port
model), the report predicate, the marker validator and the acceptance checker, each with damage controls and judge
controls.  HOST MODEL ONLY: no statement here is target evidence.

usage (Linux/WSL): test_e1.py OUT --facts-bin PATH/TO/ext_facts --pin-root REPO_WORKTREE
  OUT is created fresh.  Builds plain, ASan+UBSan (non-recovering, leaks on), TSan, TSan negative control.
Every fault case runs under plain AND ASan+UBSan, in its own base directory, and must match the flow model
(e1_model.py) EXACTLY: the marker list, the PPU call/cleanup inventory, the producer inventory and the exit status;
plus named results/fields; the validator must accept the record (only the files whose own write failed may be
INVALID).  Every input is pinned before and after the run.  Exit 0 only if every case and control passes."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import e1_model  # noqa: E402
import e1_tsan_judge  # noqa: E402

BUILD = '00e10570000beef0'
VALIDATE = HERE / 'validate_e1_markers.py'
CHECK = HERE / 'check_e1_capture.py'
POLL = ('p.drain', 'p.lane_stats', 'p.gpu_stats')
POLL_MAX = 20000
failed = []


def run(cmd, log, env=None, timeout=180):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env, timeout=timeout)
    Path(log).write_text(' '.join(map(str, cmd)) + f'\n\nrc {r.returncode}\n' + r.stdout + '\n--- stderr\n' + r.stderr)
    return r


def py(*a):
    return [sys.executable, '-I', '-B', *a]


def verdict(name, ok, why=''):
    print(f'{"PASS" if ok else "FAIL"} {name}{": " + why if why and not ok else ""}', flush=True)
    if not ok:
        failed.append(name)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def pins(root, facts_bin, out, head):
    rows = []
    for base in ('sdk/libps3dkprof/src', 'sdk/include/ps3dk', 'samples/profiling/ext-probe', 'tools/ps3dk-prof/src',
                 'tools/ps3dk-prof/examples', 'samples/gcm/common', 'tests/host/ps3dk-prof-spu-int'):
        for p in sorted((root / base).rglob('*')):
            if p.is_file():
                if '__pycache__' in p.parts:
                    rows.append(f'FORBIDDEN {p.relative_to(root)}')
                    continue
                rows.append(f'{sha(p)}  {p.relative_to(root)}')
    rows.append(f'{sha(facts_bin)}  facts-bin')
    rows.append(f'tracked base (git HEAD, read by the caller): {head}')
    for c in (['cc', '--version'], [sys.executable, '--version']):
        r = subprocess.run(c, capture_output=True, text=True)
        rows.append(f'{c[0]}: {(r.stdout or r.stderr).splitlines()[0]}')
    Path(out).write_text('\n'.join(rows) + '\n')
    return rows


def markers(rundir):
    out = {}
    for p in sorted(rundir.glob('extp-*-*.mk')):
        s = int(p.name[-5:-3])
        t = p.read_text(errors='replace').rstrip('\n')
        body = t[:t.rfind(' len=')]
        mr = re.search(r' result=(\S+) ', body)
        if not mr or ' counts=' not in body:  # an empty or torn record (its own write failed)
            out[s] = ('UNPARSED', {}, t)
            continue
        tail = body.split(' counts=', 1)[1].split(' ', 1)
        out[s] = (mr.group(1), dict(x.split('=', 1) for x in tail[1].split()) if len(tail) > 1 else {}, body)
    return out


# ---- trace -> inventories ----
PPU_RX = [(re.compile(r'^(p\.[a-z_]+) \d+ -> '), lambda m: m.group(1)),
          (re.compile(r'^spu (spu-init|import|group-create|thread-init|group-start) -> '), lambda m: m.group(1)),
          (re.compile(r'^spu-mb-write '), lambda m: 'mb'),
          (re.compile(r'^cleanup (terminate|join|destroy|image-close) -> '), lambda m: m.group(1)),
          (re.compile(r'^gcm (host-buffer|init) -> '), lambda m: 'gcm ' + m.group(1)),
          (re.compile(r'^gcm-flush \d+'), lambda m: 'gcm-flush'),
          (re.compile(r'^gcm-finish '), lambda m: 'gcm-finish')]
SPU_RX = [(re.compile(r'^w\.attach \d+ -> '), lambda m: 'w.attach'),
          (re.compile(r'^(w\.(?:flush|dec|fini) \d+) -> '), lambda m: m.group(1)),
          (re.compile(r'^spu-report (\d+) '), lambda m: f'report {m.group(1)}'),
          (re.compile(r'^spu-mbox-read '), lambda m: 'mbox-read')]


def inventories(trace):
    ppu, spu, polls = [], [], 0
    for ln in trace.splitlines():
        for rx, f in PPU_RX:
            m = rx.match(ln)
            if m:
                t = f(m)
                if t in POLL:
                    polls += 1
                else:
                    ppu.append(t)
                break
        for rx, f in SPU_RX:
            m = rx.match(ln)
            if m:
                spu.append(f(m))
                break
    return ppu, spu, polls


def probe(exe, out, name, faults, tsan=False):
    base = out / name
    base.mkdir()
    env = dict(os.environ, E1_BASE_DIR=str(base / 'b'), E1_TRACE=str(base / 'trace.txt'), E1_FAULTS=faults,
               ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=0',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    cmd = ['setarch', 'x86_64', '-R', exe] if tsan else [exe]  # TSan needs ASLR off on this kernel
    r = run(cmd, base / 'run.log', env=env)
    runs = sorted((base / 'b').glob('*')) if (base / 'b').exists() else []
    rd = runs[0] if len(runs) == 1 else None
    trace = (base / 'trace.txt').read_text() if (base / 'trace.txt').exists() else ''
    return r, rd, (markers(rd) if rd else {}), trace


SAN_BAD = ('ThreadSanitizer', 'AddressSanitizer', 'LeakSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer',
           'FATAL', 'Segmentation fault')


def validator_judge(stdout, torn):
    """the validator accepted the record: only the files whose own write failed (torn) are INVALID, nothing else"""
    inval, other = [], []
    for l in stdout.splitlines():
        m = re.match(r'INVALID extp-[0-9a-f]{16}-([0-9]{2})\.mk: ', l)
        if m:
            inval.append(int(m.group(1)))
        elif l.startswith(('INVALID', 'INCONSISTENT', 'ignored', 'no marker')):
            other.append(l)
    return sorted(inval) == sorted(torn) and not other


def case(exes, out, name, faults, F, results=None, fields=None, torn=(), no99=False, written=()):
    """one fault case under every build in exes; the expectation is the flow model's (no99: the 99 record's own
    open fails, so no 99 file exists)"""
    marks, ppu_w, spu_w, rc_w = e1_model.flow(F)
    reached_w = sorted((set(marks) | set(torn) | set(written)) - ({99} if no99 else set()))
    rd_plain = None
    for tag, exe in exes:
        r, rd, m, trace = probe(exe, out, f'{name}@{tag}', faults)
        why = []
        if r.returncode != rc_w:
            why.append(f'exit {r.returncode} != {rc_w}')
        if sorted(m) != reached_w:
            why.append(f'REACHED {sorted(m)} != {reached_w}')
        ppu, spu, polls = inventories(trace)
        if ppu != ppu_w:
            why.append(f'PPU inventory {ppu} != {ppu_w}')
        if spu != spu_w:
            why.append(f'producer inventory {spu} != {spu_w}')
        if polls > POLL_MAX:
            why.append(f'{polls} polling calls > {POLL_MAX}')
        for s, want in (results or {}).items():
            got = m.get(s, (None,))[0]
            if got is None or not re.fullmatch(want, got):
                why.append(f'{s:02d} result {got} !~ {want}')
        for s, kv in (fields or {}).items():
            for k, want in kv.items():
                got = m.get(s, (None, {}))[1].get(k)
                if got != want:
                    why.append(f'{s:02d} {k}={got} != {want}')
        if any(b in r.stderr for b in SAN_BAD) or (r.stderr.strip() and tag != 'plain'):
            why.append(f'sanitizer/stderr output: {r.stderr[:200]!r}')
        if rd is not None:
            v = run(py(VALIDATE, rd, '--build', BUILD), out / f'{name}@{tag}' / 'validate.log')
            if v.returncode != (1 if torn else 0) or not validator_judge(v.stdout, torn):
                why.append(f'validator exit {v.returncode}: ' + '; '.join(l for l in v.stdout.splitlines()
                                                                          if not l.startswith('VALID'))[:300])
        elif marks or torn:
            why.append('no run directory')
        verdict(f'case {name} [{tag}]', not why, '; '.join(why))
        if tag == 'plain':
            rd_plain = rd
    return rd_plain


def fix_len(text):
    """recompute len= after a semantic damage (so the damage, not the length, is what the validator sees)"""
    body = text.rstrip('\n')
    body = body[:body.rfind(' len=')]
    return f'{body} len={len(body.encode())}\n'


def mk_edit(d, seq, old, new, keep_len=False):
    p = next(d.glob(f'extp-*-{seq:02d}.mk'))
    t = p.read_text()
    if old not in t:
        return False
    t = t.replace(old, new, 1)
    p.write_text(t if keep_len else fix_len(t))
    return True


def named_judge(stdout, fname, reason):
    """exactly ONE failure line, about fname, carrying the named reason"""
    bad = [l for l in stdout.splitlines() if l.startswith(('INVALID', 'INCONSISTENT'))]
    return len(bad) == 1 and fname in bad[0] and reason in bad[0]


def e1_fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def swap_epoch_rows(text):
    rows = [l for l in text.splitlines() if l.startswith('epochrec')]
    return text.replace(rows[0], '@@').replace(rows[1], rows[0]).replace('@@', rows[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', type=Path)
    ap.add_argument('--facts-bin', type=Path, required=True)
    ap.add_argument('--pin-root', type=Path, required=True)
    ap.add_argument('--pin-head', required=True, help='the worktree HEAD (WSL git cannot read a Windows worktree)')
    a = ap.parse_args()
    out = a.out
    out.mkdir()
    before = pins(a.pin_root, a.facts_bin, out / 'pins-before.txt', a.pin_head)
    verdict('no forbidden input files (__pycache__)', not any(r.startswith('FORBIDDEN') for r in before))
    fsha = sha(a.facts_bin)
    # ---- builds ----
    for s in ('plain', 'asan', 'tsan', 'tsan-neg'):
        b = run(['bash', HERE / 'e1-build.sh', out / 'build', s], out / f'build-{s}.log', timeout=900)
        verdict(f'build {s}', b.returncode == 0, b.stdout[-400:])
    rt = run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', HERE / 'e1_report_test.c', '-o', out / 'e1rt'],
             out / 'build-report-test.log')
    verdict('build report test', rt.returncode == 0)
    if failed:
        return 1
    r = run([out / 'e1rt'], out / 'report-test.log')
    verdict('report predicate', r.returncode == 0 and re.search(r'^E1-REPORT PASS \d+$', r.stdout, re.M) is not None
            and 'FAIL' not in r.stdout, r.stdout[-300:])
    exes = [('plain', out / 'build' / 'e1-plain'), ('asan', out / 'build' / 'e1-asan')]
    cs = out / 'cases'
    cs.mkdir()

    # ---- the clean run ----
    clean = case(exes, cs, 'clean', '', {}, {i: 'ok' for i in range(1, 29) if i != 13} | {13: 'not-needed'},
                 {7: {'spu_ev1': '132', 'fail': 'none'}, 12: {'spu_ev2': '132', 'dec': 'ok:0', 'fail': 'none'},
                  16: {'events': '264', 'lost': '0', 'final': '1'}, 21: {'frames': '30', 'events_retired': '90'},
                  25: {'released': '1'}})
    # ---- the run directory: no marker at all ----
    for nm, fl in (('mkdir-run-exists', 'mkdir.run=exist'), ('mkdir-base-fails', 'mkdir.base=0000000d')):
        for tag, exe in exes:
            r, rd, m, trace = probe(exe, cs, f'{nm}@{tag}', fl)
            ppu, spu, _ = inventories(trace)
            verdict(f'case {nm} [{tag}]', r.returncode == 2 and not m and not ppu and not spu and
                    not any(b in r.stderr for b in SAN_BAD), f'rc {r.returncode} markers {sorted(m)} calls {ppu}')

    def C(*x, **k):
        return case(exes, cs, *x, **k)
    # ---- init / thread / setup ----
    C('sink-path-fails', 'p.set_sink_path=fffffffb', {'sink': 1}, {2: 'error:sink-path:fffffffb'})
    C('init-fails', 'p.init_ex=fffffffb', {'init': 1}, {2: 'error:init-ex:fffffffb'})
    C('thread-begin-fails', 'p.thread_begin=fffffff4', {'tb': 1}, {3: 'error:thread-begin:fffffff4'})
    C('register-fails', 'p.register=fffffffb', {'reg': 1}, {4: 'error:register-elf:fffffffb'})
    C('lane-create-fails', 'p.lane_create=fffffff0', {'lc': 1}, {4: 'error:lane-create:fffffff0'})
    for k, rel in (('spu-init', 'not-attempted:not-created'), ('import', 'not-attempted:not-created'),
                   ('group-create', 'ok'), ('thread-init', 'ok')):
        C(f'setup-{k}-fails', f'setup.{k}=80010001', {'setup': k},
          {4: f'error:{k}:80010001', 14: 'not-attempted:not-started', 16: 'not-attempted:not-joined', 17: rel},
          {4: {'lane': '1'}})
    C('setup-group-create-then-image-close-fails', 'setup.group-create=80010001,cleanup.image-close=80010008',
      {'setup': 'group-create', 'close': 1}, {17: 'error:image-close:80010008'})
    C('group-start-fails', 'setup.group-start=80010002', {'start': 1},
      {6: 'error:group-start:80010002', 13: 'ok', 14: 'ok', 15: 'ok'})
    # ---- report 1 ----
    for fl, res in (('stale', 'timeout'), ('nocommit', 'timeout'), ('identity', 'identity'), ('torn', 'identity')):
        C(f'report1-{fl}', f'rep1={fl}', {'rep1': 'timeout' if fl in ('stale', 'nocommit') else 'identity'},
          {7: res, 13: 'ok', 15: 'ok', 16: 'ok'})
    C('attach-fails', 'w.attach=fffffffb', {'attach': 1}, {7: 'spu-status:attach:-5', 13: 'not-needed'},
      {7: {'spu_ev1': '0', 'fail': 'attach:-5'}})
    C('epoch1-flush-fails', 'w.flush@3=ffffffeb', {'flush': 3}, {7: 'spu-status:flush:-21'}, {7: {'spu_ev1': '49'}})
    C('epoch1-event-fault', 'w.badidx@2', {'badidx': 2}, {7: 'spu-status:event:-21'}, {7: {'spu_ev1': '49'}})
    # ---- drain ----
    C('drain-timeout', 'p.lane_stats=stuck', {'drain': 'timeout'}, {9: 'timeout', 13: 'ok', 16: 'ok'})
    C('drain-fails', 'p.drain@1=fffffff1', {'drain': 'error'}, {9: 'error:drain:fffffff1', 16: 'not-attempted:ext-fault'})
    C('lane-stats-fails', 'p.lane_stats@1=fffffff1', {'drain': 'stats-error'},
      {9: 'error:stats:fffffff1', 16: 'not-attempted:ext-fault'})
    # ---- GO and report 2 ----
    C('go-mbox-error', 'mb=err:80010013', {'mb': 'error'}, {11: 'mbox-error:80010013', 13: 'ok'})
    C('go-wrong-value', 'mb=value:12345678', {'mb': 'value'}, {12: 'spu-status:go:-1', 13: 'not-needed'},
      {12: {'spu_ev2': '0', 'dec': 'not-attempted', 'seen_ridx': 'unavailable'}})
    for fl, res in (('stale', 'timeout'), ('identity', 'identity'), ('torn', 'identity')):
        C(f'report2-{fl}', f'rep2={fl}', {'rep2': 'timeout' if fl == 'stale' else 'identity'}, {12: res, 13: 'ok'})
    C('first-post-go-flush-fails', 'w.flush@10=ffffffeb', {'flush': 10}, {12: 'spu-status:flush:-21'},
      {12: {'dec': 'not-attempted', 'seen_ridx': 'unavailable', 'spu_ev2': '0'}})
    C('dec-eagain-then-ok', 'w.dec@1=ffffffec', {'dec': 'eagain'}, {12: 'ok'}, {12: {'dec': 'ok:0'}})
    C('dec-efault', 'w.dec@1=ffffffeb', {'dec': 'efault'}, {12: 'spu-status:dec:-21'},
      {12: {'dec': 'failed:-21', 'spu_ev2': '0'}})
    C('epoch2-flush-fails', 'w.flush@13=ffffffea', {'flush': 13}, {12: 'spu-status:flush:-22'},
      {12: {'dec': 'ok:0', 'spu_ev2': '49'}})
    C('fini-fails', 'w.fini=ffffffeb', {'fini': 1}, {12: 'spu-status:fini:-21'}, {12: {'dec': 'ok:0', 'spu_ev2': '132'}})
    # ---- SPU cleanup ----
    C('terminate-fails', 'rep1=stale,cleanup.terminate=80010005', {'rep1': 'timeout', 'term': 1},
      {13: 'error:terminate:80010005', 14: 'not-attempted:terminate-failed', 16: 'not-attempted:not-joined',
       17: 'not-attempted:not-joined'})
    C('join-fails', 'cleanup.join=80010006', {'join': 1},
      {15: 'error:join:80010006', 16: 'not-attempted:not-joined', 17: 'not-attempted:not-joined'})
    C('lane-end-fails', 'p.lane_end=ffffffed', {'lane_end': 1}, {16: 'error:lane-end:ffffffed'})
    C('destroy-fails', 'cleanup.destroy=80010007', {'destroy': 1}, {17: 'error:destroy:80010007'})
    C('image-close-fails', 'cleanup.image-close=80010008', {'close': 1}, {17: 'error:image-close:80010008'})
    # ---- GPU ----
    C('host-buffer-fails', 'gcm.host-buffer=80410002', {'hb': 1}, {18: 'unavailable:host-buffer:80410002'})
    C('gcm-init-fails', 'gcm.init=80410001', {'gcm': 1}, {18: 'unavailable:gcm-init:80410001'})
    C('gpu-attach-fails', 'p.gpu_attach=fffffffb', {'gattach': 1}, {19: 'error:gpu-attach:fffffffb'})
    C('frame-timeout', 'gpu.drop@5', {'frame_fail': (5, 'timeout')}, {21: 'timeout'},
      {21: {'frames': '4', 'events_retired': '12'}})
    # after a failed END or MARK the frame's already-emitted commands were never flushed by the probe, so the domain
    # cannot finalize and detach times out (the domain stays borrowed); after a failed frame/begin nothing is pending
    for what, k, res, det in (('frame', 4, 'error:frame:fffffffb', 'ok'), ('begin', 3, 'error:begin:fffffffb', 'ok'),
                              ('end', 7, 'error:end:fffffffb', 'timeout'), ('mark', 30, 'error:mark:fffffffb', 'timeout')):
        C(f'gpu-{what}-fails', f'p.gpu_{what}@{k}=fffffffb', {'frame_fail': (k, what)}, {21: res, 22: det},
          {21: {'frames': str(k - 1)}})
    C('gpu-stats-fails', 'p.gpu_stats@1=fffffff1', {'frame_fail': (1, 'stats')}, {21: 'error:stats:fffffff1'},
      {21: {'frames': '0'}})
    C('detach-timeout', 'p.gpu_detach=ffffffed', {'detach': 'timeout'}, {22: 'timeout'})
    C('detach-fails', 'p.gpu_detach=fffffff1', {'detach': 'error'}, {22: 'error:detach:fffffff1'})
    C('release-fails', 'p.gpu_release=fffffffe', {'release': 1}, {25: 'error:release:fffffffe'}, {25: {'released': '0'}})
    C('shutdown-fails', 'p.shutdown=fffffff6', {'shutdown': 1}, {27: 'error:shutdown:fffffff6'})
    C('capture-open-fails', 'cap.open=00000002', {'cap': 'open'}, {28: 'error:open:00000002'})
    C('capture-read-fails', 'cap.read=00000005', {'cap': 'read'}, {28: 'error:read:00000005'})
    # ---- logging failures at EVERY marker of the clean path (its open fails: no file), plus write/fsync/close ----
    for n in range(1, 29):
        C(f'log-fail-{n:02d}-open', f'mk{n:02d}.open=0000001c', {'log': n}, {99: 'failed'})
    C('log-fail-01-write-zero', 'mk01.write=zero', {'log': 1}, {99: 'failed'}, torn=[1])
    C('log-fail-10-write', 'mk10.write=00000005', {'log': 10}, {99: 'failed'}, torn=[10])
    C('log-fail-18-fsync', 'mk18.fsync=00000005', {'log': 18}, {99: 'failed'}, written=[18])  # complete file, fsync failed: VALID
    C('log-fail-20-close', 'mk20.close=00000005', {'log': 20}, {99: 'failed'}, written=[20])  # complete file, close failed: VALID
    C('log-fail-07-99-also-fails', 'mk07.open=0000001c,mk99.open=0000001c', {'log': 7}, no99=True)

    # ---- TSan: ONE shared judge (e1_tsan_judge.judge, the reviewed I2 strict parser) for the live rows and controls ----
    tx, ts = out / 'build' / 'e1-tsan', out / 'tsan'
    ts.mkdir()
    for nm, fl, F in (('tsan-clean', '', {}), ('tsan-report1-torn', 'rep1=torn', {'rep1': 'identity'}),
                      ('tsan-report2-torn', 'rep2=torn', {'rep2': 'identity'})):
        marks, ppu_w, spu_w, rc_w = e1_model.flow(F)
        r, rd, m, trace = probe(tx, ts, nm, fl, tsan=True)
        v, why = e1_tsan_judge.judge('clean', r.returncode, False, m, marks, r.stderr, rc_w)
        verdict(f'tsan {nm} CLEAN', v == 'CLEAN' and inventories(trace)[:2] == (ppu_w, spu_w), f'{v}: {why}')
    r, rd, m, trace = probe(out / 'build' / 'e1-tsan-neg', ts, 'tsan-negative-control', '', tsan=True)
    v, why = e1_tsan_judge.judge('negative', r.returncode, False, m, e1_model.flow({})[0], r.stderr)
    verdict('tsan negative control REPORTED', v == 'REPORTED', f'{v}: {why}')
    j = run(py(HERE / 'e1_tsan_judge.py', '--selftest', ts / 'tsan-negative-control' / 'run.log'),
            out / 'tsan-judge-selftest.log')
    verdict('tsan judge selftest (controls built from this run\'s real report)',
            j.returncode == 0 and re.search(r'^E1-TSAN-JUDGE PASS \d+/\d+$', j.stdout, re.M) is not None, j.stdout[-300:])

    # ---- the lane OBSERVATION wording must state the current epoch rule ----
    # both cases (raw_valid 0 and 1): a depth-zero epoch change keeps the lane known;
    # the history is unverifiable only after a loss or at non-zero depth.  The
    # self-test is a single-sourced assertion inside check_e1_capture.py.
    ow = run(py(CHECK, '--selftest-observation'), out / 'obs-wording.log')
    verdict('observation wording: both cases state the depth-zero rule',
            ow.returncode == 0 and re.search(r'^E1-OBSERVATION-WORDING PASS 2/2$', ow.stdout, re.M) is not None,
            ow.stdout[-300:])
    # mutation control: revert the stale "NOT VALID expected" claim and the
    # self-test must reject it (not vacuous)
    src = CHECK.read_text()
    reverted = src.replace(
        "return f'OBSERVATION lane {lane_id}: raw_valid={raw_valid} ({LANE_VALID_NOTE})'",
        "return f'OBSERVATION lane {lane_id}: raw_valid={raw_valid} (NOT VALID expected by the epoch rule)'")
    if reverted != src:
        p = out / 'check_stale.py'
        p.write_text(reverted)
        mut = run(py(p, '--selftest-observation'), out / 'obs-wording-mut.log')
        verdict('wording self-test rejects the stale-wording revert (not vacuous)',
                mut.returncode == 1 and 'NOT VALID expected' in mut.stdout,
                mut.stdout[-200:])
    else:
        verdict('wording self-test rejects the stale-wording revert (not vacuous)',
                False, 'mutation did not apply')

    # ---- acceptance: the clean run, then its damage controls (the checker runs the reader itself) ----
    acc = out / 'accept'
    acc.mkdir()

    def accept(d, reader, rsha, capture, log):
        cmd = py(CHECK, d, '--build', BUILD, '--facts-bin', reader, '--facts-bin-sha256', rsha)
        if capture:
            cmd += ['--capture', capture]
        return run(cmd, acc / f'{log}.log')
    if clean is None:
        verdict('clean run directory', False)
        return 1
    c = accept(clean, a.facts_bin, fsha, None, 'clean')
    verdict('acceptance clean ACCEPT', c.returncode == 0 and c.stdout.rstrip().endswith('ACCEPT'), c.stdout[-400:])
    good = subprocess.run([a.facts_bin, next(clean.glob('extp-*.prof'))], capture_output=True, text=True).stdout
    dmg = out / 'damage'
    dmg.mkdir()

    def fake_reader(name, text, rc=0, err=''):
        p = dmg / f'reader-{name}.sh'
        (dmg / f'reader-{name}.txt').write_text(text)
        p.write_text(f'#!/bin/sh\ncat "{dmg}/reader-{name}.txt"\n' + (f'echo "{err}" >&2\n' if err else '') +
                     f'exit {rc}\n')
        p.chmod(0o755)
        return p

    def reject(name, d=clean, reader=None, rsha=None, capture=None, must=''):
        rd_ = reader or a.facts_bin
        c = accept(d, rd_, rsha or sha(rd_), capture, name)
        verdict(f'acceptance control {name} REJECT', c.returncode == 1 and must in c.stdout, c.stdout[-300:])

    # point 1: bound to the capture
    junk = b'not a profiler capture\n'
    d = dmg / 'wrong-capture-matching-readback'
    shutil.copytree(clean, d)
    next(d.glob('extp-*.prof')).write_bytes(junk)
    t28 = next(d.glob('extp-*-28.mk'))
    t28.write_text(fix_len(re.sub(r'size=\d+ fnv=[0-9a-f]{16}', f'size={len(junk)} fnv={e1_fnv(junk):016x}',
                                  t28.read_text())))
    reject('wrong-capture-matching-readback', d, must='facts: reader exit')
    other = next((cs / 'dec-eagain-then-ok@plain' / 'b').glob('*/extp-*.prof'))
    reject('different-capture', clean, capture=other, must='28 size/fnv')
    reject('failed-reader-plausible-stdout', reader=fake_reader('rc1', good, rc=1), must='reader exit 1')
    reject('reader-stderr', reader=fake_reader('stderr', good, err='warning: something'), must='reader stderr')
    reject('truncated-reader-output', reader=fake_reader('trunc', good[:good.rfind('end\n')]), must='truncated')
    reject('reader-not-pinned', rsha='0' * 64, must='not the pinned binary')
    # point 2: the closed inventory (a reader printing the clean facts with one change)
    lane_ev = [l for l in good.splitlines() if l.startswith('events kind=lane')][0]
    var2 = [l for l in good.splitlines() if l.startswith('variation')]
    inv = {
        'dangling-event-row': (good.replace('end\n', 'events kind=lane id=999 epoch=1 ev=mark desc=unexpected count=1\nend\n'),
                               'events rows'),
        'duplicate-event-row': (good.replace(lane_ev + '\n', lane_ev + '\n' + lane_ev + '\n', 1), 'duplicates'),
        'missing-observation-rows': (''.join(l + '\n' for l in good.splitlines() if not l.startswith('variation')),
                                     'variation rows'),
        'one-observation-row-missing': (good.replace(var2[1] + '\n', '') if len(var2) == 2 else good, 'variation rows'),
        'extra-epoch-row': (good.replace('end\n', 'epochrec domain=2 epoch=3 reason=2 written=00000000ffffffff\nend\n'),
                            'epochrec rows'),
        'epoch-rows-reordered': (swap_epoch_rows(good), 'epochrec rows'),
        'epoch-reason-wrong': (good.replace('epoch=2 reason=2', 'epoch=2 reason=1', 1), 'epochrec rows'),
        'file-incomplete-count': (good.replace('verdict=complete incomplete=0', 'verdict=complete incomplete=1'),
                                  'incomplete=1'),
        'extra-source': (good.replace('end\n', 'source kind=lane id=2 domain=3 structure=complete raw_valid=1 final=1 '
                                      'events=0 lost=0 losses=0 epochs=1 paired=0 marks=0 unpaired=0 censored=0 '
                                      'frames_resolved=0 frames_unresolved=0 frames_none=0\nend\n'), 'source rows'),
        'lane-263-events': (good.replace('events=264', 'events=263', 1), 'events=263'),
        'lane-no-final': (good.replace('raw_valid=1 final=1', 'raw_valid=1 final=none', 1), 'final=none'),
        'gpu-89-events': (good.replace('raw_valid=1 final=1 events=90', 'raw_valid=1 final=1 events=89'), 'events=89'),
        'epoch1-only': (good.replace('events kind=lane id=1 epoch=2 ev=begin desc=probe-e2 count=64\n', ''), 'events rows'),
        'wrong-descriptor': (good.replace('epoch=1 ev=mark desc=probe-mark count=4', 'epoch=1 ev=mark desc=probe-e1 count=4'),
                             'events rows'),
        'no-epoch-write': (good.replace('written=00000000ffffffff', 'written=0000000000000000'), 'epochrec rows'),
        'unparsed-line': (good.replace('end\n', 'something new=1\nend\n'), 'unparsed line'),
        'loss-record': (good.replace('lost=0 losses=0', 'lost=0 losses=1', 1), 'losses=1'),
        'frames-unresolved': (good.replace('frames_resolved=30 frames_unresolved=0',
                                           'frames_resolved=29 frames_unresolved=1'), 'frames_resolved=29'),
        'malformed-file-line': ('E1FACTS 1\nfile malformed=1 rule=Extension\nend\n', 'unparsed line'),
    }
    for name, (text, must) in inv.items():
        if text == good:
            verdict(f'acceptance control {name} REJECT', False, 'the damage did not apply')
            continue
        reject(name, reader=fake_reader(name, text), must=must)
    ridx = markers(clean)[9][1]['read_idx']
    for name, (seq, old, new, must) in {
        'seen-ridx-mismatch': (12, f'seen_ridx={ridx}', f'seen_ridx={int(ridx, 16) + 2:016x}', 'read_idx 09'),
        'read-idx-mismatch': (9, f'read_idx={ridx}', f'read_idx={int(ridx, 16) + 2:016x}', 'read_idx 09'),
        'capture-fnv': (28, 'fnv=', 'fnv=0', '28 size/fnv'),
    }.items():
        d = dmg / name
        shutil.copytree(clean, d)
        verdict(f'acceptance control {name}: damage applied', mk_edit(d, seq, old, new))
        reject(name, d, must=must)
    d = dmg / 'capture-byte-flip'
    shutil.copytree(clean, d)
    cp = next(d.glob('extp-*.prof'))
    b = bytearray(cp.read_bytes())
    b[-1] ^= 1
    cp.write_bytes(bytes(b))
    reject('capture-byte-flip', d, must='28 size/fnv')
    d = dmg / 'missing-marker'
    shutil.copytree(clean, d)
    next(d.glob('extp-*-20.mk')).unlink()
    reject('missing-marker', d, must='missing [20]')

    # ---- validator damage controls: the NAMED reason must be the only failure ----
    vd = out / 'validator-damage'
    vd.mkdir()
    l05 = int(re.search(r' len=(\d+)\n', next(clean.glob('extp-*-05.mk')).read_text()).group(1))
    vdam = {
        'len-wrong': (clean, 5, f' len={l05}\n', f' len={l05 + 1}\n', True, f'len {l05 + 1} != prefix {l05}'),
        'build-wrong': (clean, 3, 'build=00e10570000beef0', 'build=00e10570000beef1', False, 'build 00e10570000beef1'),
        'result-not-in-matrix': (clean, 6, 'result=ok', 'result=maybe', False, 'cannot carry result maybe'),
        'fields-on-timeout': (clean, 7, 'result=ok', 'result=timeout', False, 'report fields iff the report is trusted'),
        'unres-without-cause': (clean, 8, 'unres=n,n', 'unres=u,n', False, 'unres must be n'),
        'clean-report-short': (clean, 7, 'spu_ev1=132', 'spu_ev1=131', False, 'a clean report 1 covers all of epoch 1'),
        'fail-field-mismatch': (clean, 12, 'fail=none', 'fail=flush:-21', False, 'fail= must restate the result'),
        'frames-ok-short': (clean, 21, 'frames=30', 'frames=29', False, 'frames ok iff all 30 frames resolved'),
        'release-ok-unreleased': (clean, 25, 'released=1', 'released=0', False,
                                  'a successful release returned the resources'),
    }
    for name, (src, seq, old, new, keep, reason) in vdam.items():
        d = vd / name
        shutil.copytree(src, d)
        applied = mk_edit(d, seq, old, new, keep_len=keep)
        v = run(py(VALIDATE, d, '--build', BUILD), vd / f'{name}.log')
        fname = next(d.glob(f'extp-*-{seq:02d}.mk')).name
        verdict(f'validator control {name}: exactly its named reason', applied and v.returncode == 1 and
                named_judge(v.stdout, fname, reason), v.stdout[-300:])
    # a writer result the call cannot return, and a post-GO call in report 1 (review 10:25 point 3)
    for name, (src_case, edits, reason) in {
        'rc-not-a-writer-result': ('attach-fails', [('spu-status:attach:-5', 'spu-status:attach:-1'),
                                                    ('fail=attach:-5', 'fail=attach:-1')],
                                   'is not a result attach can return'),
        'report1-go-call': ('attach-fails', [('spu-status:attach:-5', 'spu-status:go:-1'), ('fail=attach:-5', 'fail=go:-1')],
                            'report 1 cannot carry a post-GO call'),
        'attach-failure-after-events': ('attach-fails', [('spu_ev1=0', 'spu_ev1=132')], 'an attach failure precedes every event'),
        'fini-without-epoch2': ('fini-fails', [('spu_ev2=132', 'spu_ev2=0')], 'not a state the producer can send for fini'),
    }.items():
        d = vd / name
        shutil.copytree(next((cs / f'{src_case}@plain' / 'b').glob('*')), d)
        applied = all(mk_edit(d, 7 if 'report1' in name or 'attach' in name or 'rc-' in name else 12, o, n)
                      for o, n in edits)
        seq = 7 if 'report1' in name or 'attach' in name or 'rc-' in name else 12
        v = run(py(VALIDATE, d, '--build', BUILD), vd / f'{name}.log')
        fname = next(d.glob(f'extp-*-{seq:02d}.mk')).name
        verdict(f'validator control {name}: exactly its named reason', applied and v.returncode == 1 and
                named_judge(v.stdout, fname, reason), v.stdout[-300:])
    # flow controls (a record the probe cannot write after the present ones): INCONSISTENT with the named rule
    d = vd / 'lane-end-after-failed-join'
    shutil.copytree(next((cs / 'join-fails@plain' / 'b').glob('*')), d)
    applied = mk_edit(d, 16, 'result=not-attempted:not-joined', 'result=ok') and \
        mk_edit(d, 16, 'counts=264,0 len=', 'counts=264,0 events=264 lost=0 final=1 len=')  # a well-formed ok record
    v = run(py(VALIDATE, d, '--build', BUILD), vd / 'lane-end-after-failed-join.log')
    verdict('validator flow control lane-end-after-failed-join: its rule', applied and v.returncode == 1 and
            any('lane_end only after a successful join' in l for l in v.stdout.splitlines() if l.startswith('INCONSISTENT')),
            v.stdout[-300:])
    # judge controls: the named-reason judge rejects a wrong reason, a second failure and no failure
    jl = 'INVALID extp-0000000000000000-06.mk: stage 06 cannot carry result maybe'
    verdict('judge rejects a wrong reason', not named_judge(jl.replace('cannot carry', 'x'), '-06.mk',
                                                            'cannot carry result maybe'))
    verdict('judge rejects a second failure', not named_judge(jl + '\nINVALID extp-0-07.mk: y', '-06.mk',
                                                              'cannot carry result maybe'))
    verdict('judge rejects no failure', not named_judge('VALID x', '-06.mk', 'cannot carry result maybe'))
    verdict('judge accepts the named failure', named_judge(jl, '-06.mk', 'cannot carry result maybe'))

    after = pins(a.pin_root, a.facts_bin, out / 'pins-after.txt', a.pin_head)
    verdict('inputs unchanged during the run (pins before == after)', before == after)
    print(f'E1 HOST TESTS {"PASS" if not failed else "FAIL"}: {len(failed)} failed', flush=True)
    for f in failed:
        print(f'  failed: {f}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
