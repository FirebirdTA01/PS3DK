#!/usr/bin/env python3
"""Host tests of the launch-marker probe and its validator.  source/main.c is built with LM_HOST_TEST (a fixed
timebase, LM_DIR a temp directory) and the syscall seam lm_seam.c (LM_FAULTS injects per-call results; LM_TRACE
records every call), plain and with non-recovering ASan/UBSan; every run must leave stderr EMPTY.  Host behaviour
only: nothing here says how LV2 or the console's filesystem behaves, and no wall-time bound is claimed for any call.
usage: test_launch_marker.py OUT  (exit 0 only if every row passes)"""
import os
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
top = here.parent
src = top / 'source/main.c'
val = here / 'validate_markers.py'
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
cc = os.environ.get('CC', 'cc')
RUN, BUILD = '0123456789abcdef', 'fedcba9876543210'
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
rows = []


def row(name, ok, info=''):
    rows.append((name, ok))
    print(f'{name}: {"PASS" if ok else "FAIL"} {info}'.rstrip(), flush=True)


def compile_(tag, d, extra=(), build=BUILD):
    exe = out / f'lm-{tag}'
    r = subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *extra, f'-I{here}',
                        f'-DLM_HOST_TEST=0x{RUN}', f'-DLM_DIR="{d}"', f'-DLM_BUILD_ID="{build}"', str(src),
                        str(here / 'lm_seam.c'), '-o', str(exe)], capture_output=True, text=True)
    (out / f'lm-{tag}.build.txt').write_text(r.stdout + r.stderr)
    return exe if r.returncode == 0 else None


def rec(seq, stage, result):
    p = f'CLKM1 run={RUN} variant=L build={BUILD} seq={seq} stage={stage} result={result} counts=0,0'.encode()
    return p + b' len=' + str(len(p)).encode() + b'\n'


def validate(d, build=BUILD):
    r = subprocess.run([sys.executable, '-I', str(val), str(d), '--build', build], capture_output=True, text=True)
    return r.returncode, r.stdout


M01, M17, M99 = (f'clockcal-{RUN}-{s}.mk' for s in ('01', '17', '99'))
OK01 = rec('01', 'main-entered', 'ok')
OK17 = rec('17', 'marker-closed', 'ok')
L01, L17 = len(OK01), len(OK17)


def e17(r):
    return rec('17', 'marker-closed', r)


def e99(r):
    return rec('99', 'logging-failed', r)


# (name, LM_FAULTS, rc, {file: bytes}, COMPLETE normalized trace, validator rc, facts)
# trace lines: "<call> <arg> -> <rc> <errno>", an open's rc normalized to "fd" when >= 0; compared for EQUALITY
def T(*lines):
    return list(lines)


def mk(seq, n):  # one marker whose open, single write, fsync and close all succeed
    return T(f'open {seq} -> fd 0', f'write {n} -> {n} 0', 'fsync -> 0 0', 'close -> 0 0')


def F(io01, logf):
    return f'MARKER01_IO={io01} LOGGING_FAILURE={logf} PROCESS_EXIT=UNOBSERVED'


ok01, ok17 = mk('01', L01), mk('17', L17)
n17 = lambda r: len(e17(r))  # noqa: E731
n99 = lambda r: len(e99(r))  # noqa: E731
CASES = [
    ('normal', '', 0, {M01: OK01, M17: OK17}, ok01 + ok17, 0, F('SUCCESS_SHOWN', 'NOT_SHOWN')),
    ('partial-then-ok', 'write=part:10,part:5', 0, {M01: OK01, M17: OK17},
     T('open 01 -> fd 0', f'write {L01} -> 10 0', f'write {L01 - 10} -> 5 0', f'write {L01 - 15} -> {L01 - 15} 0',
       'fsync -> 0 0', 'close -> 0 0') + ok17, 0, F('SUCCESS_SHOWN', 'NOT_SHOWN')),
    ('eintr-then-ok', 'write=eintr', 0, {M01: OK01, M17: OK17},
     T('open 01 -> fd 0', f'write {L01} -> -1 4', f'write {L01} -> {L01} 0', 'fsync -> 0 0', 'close -> 0 0') + ok17,
     0, F('SUCCESS_SHOWN', 'NOT_SHOWN')),
    ('zero-write', 'write=zero', 1, {M01: b'', M17: e17('error:write:zero')},
     T('open 01 -> fd 0', f'write {L01} -> 0 0', 'close -> 0 0') + mk('17', n17('error:write:zero')), 1,
     F('FAILURE_SHOWN', 'SHOWN')),
    ('error-after-partial', 'write=part:20,err:5', 1, {M01: OK01[:20], M17: e17('error:write:00000005')},
     T('open 01 -> fd 0', f'write {L01} -> 20 0', f'write {L01 - 20} -> -1 5', 'close -> 0 0') +
     mk('17', n17('error:write:00000005')), 1, F('FAILURE_SHOWN', 'SHOWN')),
    ('fsync-fail', 'fsync=err:5', 1, {M01: OK01, M17: e17('error:fsync:00000005')},
     T('open 01 -> fd 0', f'write {L01} -> {L01} 0', 'fsync -> -1 5', 'close -> 0 0') +
     mk('17', n17('error:fsync:00000005')), 0, F('FAILURE_SHOWN', 'SHOWN')),
    ('close-fail', 'close=err:9', 1, {M01: OK01, M17: e17('error:close:00000009')},
     T('open 01 -> fd 0', f'write {L01} -> {L01} 0', 'fsync -> 0 0', 'close -> -1 9') +
     mk('17', n17('error:close:00000009')), 0, F('FAILURE_SHOWN', 'SHOWN')),
    ('first-error-write-then-close', 'write=err:5;close=err:9', 1, {M01: b'', M17: e17('error:write:00000005')},
     T('open 01 -> fd 0', f'write {L01} -> -1 5', 'close -> -1 9') + mk('17', n17('error:write:00000005')), 1,
     F('FAILURE_SHOWN', 'SHOWN')),
    ('first-error-fsync-then-close', 'fsync=err:5;close=err:9', 1, {M01: OK01, M17: e17('error:fsync:00000005')},
     T('open 01 -> fd 0', f'write {L01} -> {L01} 0', 'fsync -> -1 5', 'close -> -1 9') +
     mk('17', n17('error:fsync:00000005')), 0, F('FAILURE_SHOWN', 'SHOWN')),
    ('lv2-errno-open', 'open=err:0x80010002', 1, {M17: e17('error:open:80010002')},
     T('open 01 -> -1 -2147418110') + mk('17', n17('error:open:80010002')), 0, F('FAILURE_SHOWN', 'SHOWN')),
    ('m17-fails-one-99', 'open=ok,err:28', 1, {M01: OK01, M99: e99('error:open:0000001c')},
     ok01 + T('open 17 -> -1 28') + mk('99', n99('error:open:0000001c')), 0, F('NOT_SHOWN', 'SHOWN')),
    ('m99-fails-too', 'open=ok,err:28,err:28', 1, {M01: OK01},
     ok01 + T('open 17 -> -1 28', 'open 99 -> -1 28'), 0, F('NOT_SHOWN', 'NOT_SHOWN')),
    ('m17-partial-then-99', 'write=ok,part:30,err:5', 1, {M01: OK01, M17: OK17[:30], M99: e99('error:write:00000005')},
     ok01 + T('open 17 -> fd 0', f'write {L17} -> 30 0', f'write {L17 - 30} -> -1 5', 'close -> 0 0') +
     mk('99', n99('error:write:00000005')), 1, F('NOT_SHOWN', 'SHOWN')),
    # 17 ok + 99, and the longest record: 17 complete, then its close fails with an LV2-sized errno -> 99 carries it
    ('longest-record', 'close=ok,err:0x80010002', 1, {M01: OK01, M17: OK17, M99: e99('error:close:80010002')},
     ok01 + T('open 17 -> fd 0', f'write {L17} -> {L17} 0', 'fsync -> 0 0', 'close -> -1 -2147418110') +
     mk('99', n99('error:close:80010002')), 0, F('SUCCESS_SHOWN', 'SHOWN')),
    # 17 ok, its close fails, and creating 99 fails too: the files show 01's success only; rc 1 is not visible in them
    ('m17-ok-99-not-created', 'close=ok,err:9;open=ok,ok,err:28', 1, {M01: OK01, M17: OK17},
     ok01 + T('open 17 -> fd 0', f'write {L17} -> {L17} 0', 'fsync -> 0 0', 'close -> -1 9', 'open 99 -> -1 28'), 0,
     F('SUCCESS_SHOWN', 'NOT_SHOWN')),
]


def norm(lines):
    o = []
    for ln in lines:
        p = ln.split()
        if p[0] == 'open' and int(p[3]) >= 0:
            p[3] = 'fd'
        o.append(' '.join(p))
    return o


def judge(rc, stderr, files, trace, d, want_rc, want_files, want_trace, want_vrc, facts):
    why = []
    if rc != want_rc:
        why.append(f'rc {rc}')
    if stderr:
        why.append('stderr not empty')
    if files != want_files:
        why.append(f'files {sorted(files)} / bytes differ')
    if trace != want_trace:
        why.append(f'trace {trace}')
    if files:
        vrc, vo = validate(d)
        if vrc != want_vrc:
            why.append(f'validator rc {vrc}')
        if f'run {RUN}: {facts} ' not in vo:
            why.append(f'facts not {facts}')
    return why


assert len(e99('error:close:80010002')) == 126 + len(b' len=126\n')
kept = {}
for v, extra in (('plain', []), ('asan', SAN)):
    for name, faults, want_rc, want_files, want_trace, want_vrc, facts in CASES:
        d = out / f'{name}-{v}'
        d.mkdir()
        exe = compile_(f'{name}-{v}', d, extra)
        if not exe:
            row(f'{name}-{v}', False, 'build failed')
            continue
        tr = out / f'{name}-{v}.trace'
        env = dict(os.environ, LM_FAULTS=faults, LM_TRACE=str(tr))
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        files = {p.name: p.read_bytes() for p in d.iterdir()}
        trace = norm(tr.read_text().splitlines()) if tr.exists() else []
        why = judge(r.returncode, r.stderr, files, trace, d, want_rc, want_files, want_trace, want_vrc, facts)
        if v == 'plain' and not why:
            kept[name] = (r.returncode, files, trace, d)
        row(f'{name}-{v}', not why, '; '.join(why))
# judge controls: the real run's rc, stderr, files and validator unchanged; only the trace doctored
for cname, base, doctor in (
        ('extra-fsync-at-end', 'normal', lambda t: t + ['fsync -> 0 0']),
        ('extra-write-at-end', 'normal', lambda t: t + [f'write {L17} -> {L17} 0']),
        ('reordered-close', 'normal', lambda t: t[:2] + [t[3], t[2]] + t[4:]),
        ('second-99-attempt', 'm99-fails-too', lambda t: t + ['open 99 -> -1 28']),
        ('fsync-retried', 'fsync-fail', lambda t: t[:3] + ['fsync -> 0 0'] + t[3:])):
    if base not in kept:
        row(f'judge control {cname}', False, f'no clean {base} run')
        continue
    c = next(c for c in CASES if c[0] == base)
    rc, files, trace, d = kept[base]
    why = judge(rc, '', files, doctor(list(trace)), d, c[2], c[3], c[4], c[5], c[6])
    row(f'judge control {cname}', why == [f'trace {doctor(list(trace))}'], f'rejected for: {why}')
for v, extra in (('plain', []), ('asan', SAN)):
    # real open failures: a pre-existing 01 (EEXIST) and a missing directory
    d = out / f'exists-{v}'
    d.mkdir()
    (d / M01).write_bytes(b'')
    def go(tag, d, env=None, more=()):
        exe = compile_(f'{tag}-{v}', d, [*extra, *more])
        if not exe:
            row(f'{tag}-{v}', False, 'build failed')
            return None
        return subprocess.run([str(exe)], capture_output=True, text=True, env=env)

    r = go('exists', d)
    if r:
        files = {p.name: p.read_bytes() for p in d.iterdir()}
        row(f'exists-{v}', r.returncode == 1 and not r.stderr and files == {M01: b'', M17: e17('error:open:00000011')},
            f'rc {r.returncode}')
    d = out / f'nodir-{v}'
    r = go('nodir', d)
    if r:
        row(f'nodir-{v}', r.returncode == 1 and not r.stderr and not d.exists(), f'rc {r.returncode}')
    # a path that cannot be formatted: no call at all, rc 1
    tr = out / f'longpath-{v}.trace'
    # (GCC proves this truncation at compile time: its warning is disabled for this one build only)
    r = go('longpath', out / ('p' * 520), dict(os.environ, LM_TRACE=str(tr)), ['-Wno-format-truncation'])
    if r:
        row(f'longpath-{v}', r.returncode == 1 and not r.stderr and not tr.exists(), f'rc {r.returncode}')

# ---- the build tag bound: CMake, producer, validator ----
for idv, ok in (('0123456789abcdef', True), ('0123456789abcde', False), ('0123456789abcdef0', False),
                ('0123456789ABCDEF', False), ('', False)):
    r = subprocess.run(['cmake', f'-DLM_BUILD_ID={idv}', '-P', str(top / 'lm_build_id.cmake')],
                       capture_output=True, text=True)
    row(f'cmake id {idv or "(empty)"}', (r.returncode == 0) == ok, f'rc {r.returncode}')
for idv, ok in (('0123456789abcdef', True), ('0123456789abcde', False), ('0123456789abcdef0', False)):
    exe = compile_(f'id-{len(idv)}', out / 'unused', [], build=idv)
    row(f'producer id length {len(idv)}', (exe is not None) == ok)
    vr = subprocess.run([sys.executable, '-I', str(val), str(out), '--build', idv], capture_output=True, text=True)
    if not ok:
        row(f'validator --build length {len(idv)}', vr.returncode == 2, f'rc {vr.returncode}')

# ---- validator controls (constructed records) ----
def raw(seq, stage, result, build=BUILD):
    p = f'CLKM1 run={RUN} variant=L build={build} seq={seq} stage={stage} result={result} counts=0,0'.encode()
    return p + b' len=' + str(len(p)).encode() + b'\n'


CTL = [
    ('truncated', M01, OK01[:-1], 'bad len field or terminator'),
    ('extra-byte', M01, OK01 + b'x', 'bad len field or terminator'),
    ('crlf', M01, OK01[:-1] + b'\r\n', 'bad len field or terminator'),
    ('empty', M01, b'', 'no len field'),
    ('len-value', M01, OK01[:OK01.rfind(b'=') + 1] + b'1\n', f'len 1 != prefix {L01 - 9}'),
    ('len-leading-zero', M01, OK01.replace(b' len=', b' len=0'), 'bad len field or terminator'),
    ('body-garbage', M01, None, 'record does not parse'),
    ('other-build', M01, raw('01', 'main-entered', 'ok', '0000000000000000'), f'build 0000000000000000 != {BUILD}'),
    ('seq-name', f'clockcal-{RUN}-02.mk', OK01, 'run/seq differ from the file name'),
    ('unknown-stage', M01, raw('01', 'marker-closed', 'ok'), 'unknown milestone 01 marker-closed'),
    ('m01-error', M01, raw('01', 'main-entered', 'error:open:00000002'), 'marker 01 cannot carry result'),
    ('m99-ok', M99, raw('99', 'logging-failed', 'ok'), 'marker 99 cannot carry result ok'),
    ('errno-short', M17, raw('17', 'marker-closed', 'error:open:5'), 'record does not parse'),
    ('errno-long', M17, raw('17', 'marker-closed', 'error:open:000000051'), 'record does not parse'),
    ('errno-upper', M17, raw('17', 'marker-closed', 'error:open:8001000A'), 'record does not parse'),
    ('fsync-zero', M17, raw('17', 'marker-closed', 'error:fsync:zero'), 'record does not parse'),
    ('format-other', M17, raw('17', 'marker-closed', 'error:format:other'), 'record does not parse'),
    ('bad-result', M17, raw('17', 'marker-closed', 'maybe'), 'record does not parse'),
]
for name, fn, data, why in CTL:
    d = out / f'val-{name}'
    d.mkdir()
    if name == 'body-garbage':
        p = OK01[:OK01.rfind(b' len=')] + b'x'
        data = p + b' len=' + str(len(p)).encode() + b'\n'
    (d / fn).write_bytes(data)
    vr, vo = validate(d)
    row(f'validator {name}', vr == 1 and f'INVALID {fn}: {why}' in vo and '\nVALID' not in '\n' + vo, f'rc {vr}')
# outcome separation
for name, files, outcome in (('only-01', {M01: OK01}, F('NOT_SHOWN', 'NOT_SHOWN')),
                             ('17-error', {M17: e17('error:fsync:00000005')}, F('FAILURE_SHOWN', 'SHOWN')),
                             ('only-17-ok', {M17: OK17}, F('SUCCESS_SHOWN', 'NOT_SHOWN')),
                             ('17-ok-and-99', {M17: OK17, M99: e99('error:fsync:00000005')}, F('SUCCESS_SHOWN', 'SHOWN')),
                             ('99-alone', {M99: e99('error:open:0000001c')}, F('NOT_SHOWN', 'SHOWN'))):
    d = out / f'outcome-{name}'
    d.mkdir()
    for fn, b in files.items():
        (d / fn).write_bytes(b)
    vr, vo = validate(d)
    row(f'facts {name}', vr == 0 and f'run {RUN}: {outcome} ' in vo, f'rc {vr}')
d = out / 'val-none'
d.mkdir()
(d / 'clockcal-000003306df7e4cd.csv').write_bytes(b'x')
vr, vo = validate(d)
row('validator no-markers', vr == 2 and 'nothing to state' in vo, f'rc {vr}')

bad = [n for n, ok in rows if not ok]
print(f'LAUNCH-MARKER HOST {"PASS" if not bad else "FAIL"}: {len(rows)} rows, {len(bad)} failed {bad if bad else ""}'.rstrip())
sys.exit(1 if bad else 0)
