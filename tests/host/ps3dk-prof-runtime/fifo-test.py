#!/usr/bin/env python3
"""FIFO wrap phase events (libps3dkprof_ext_fifo) on the host: build the real core, extension and FIFO runtime with
the release identity pipeline, the GCM host model (real protocol and bridge headers) and fifo_test.c; run every
control; decode the capture bytes here (a minimal strict decoder of chunk types 31-33, not the reader) and, with
--reader, also check each capture with the reader.  Contract: docs/design/ps3dk-prof-fifo.md.

Mutants break one rule in a copy of the sources; the control named for it must then fail:
  relaxed-pub    the owner publishes write_idx with a relaxed store       -> TSan race in `pace`
  early-reuse    the drainer releases read_idx before encoding the slots  -> TSan race in `race`
  ack-early      the drainer releases slots after a refused chunk         -> `shortwrite` fails
  owner-drain    APP-mode (owner) draining is admitted                    -> `app` fails
The independent-progress controls: `parked1`/`parked2` hold GET short of the phase target while the drain thread must
persist the open phase's boundaries; `gpuzone` does the same with the wrap inside a GPU zone's reserve (the owner
pinned in a GPU call), the realistic held state: the owner holds no profiler mutex across any GCM reserve."""
import argparse, json, os, shutil, struct, subprocess, zlib
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--sanitize', choices=['address', 'thread'])
ap.add_argument('--case', action='append', help='run only these cases (repeatable)')
ap.add_argument('--reader', type=Path, help='a built ps3dk-prof; each capture is also checked by it')
ap.add_argument('--mutant', choices=['relaxed-pub', 'early-reuse', 'ack-early', 'owner-drain'])
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
here = Path(__file__).resolve().parent
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
src = root / 'sdk/libps3dkprof/src'
MUTANTS = {
    'relaxed-pub': ('fifo/fifo_runtime.c', [('ST_REL(&f->write_idx, w + 1u);', 'ST_RLX(&f->write_idx, w + 1u);')],
                    'pace', 'thread'),
    'early-reuse': ('fifo/fifo_writer.c', [('    while (f->d_read < w) {', '    ST_REL(&f->read_idx, w);\n    while (f->d_read < w) {'),
                                          ('        ST_REL(&f->read_idx, f->d_read); /* only now may the owner reuse them */\n', '')],
                    'race', 'thread'),
    'ack-early': ('fifo/fifo_writer.c', [('        if (p_fifo_emit_chunk(p, C_FIFO_EVENTS, b, 16u + (size_t)n * FIFO_REC_BYTES) != 0)\n'
                                          '            return -1; /* not accepted: the slots stay owned by the drainer, never reused */\n',
                                          '        (void)p_fifo_emit_chunk(p, C_FIFO_EVENTS, b, 16u + (size_t)n * FIFO_REC_BYTES);\n')],
                  'shortwrite', None),
    'owner-drain': ('fifo/fifo_runtime.c', [('    if (p->cfg.mode != PS3DK_PROF_MODE_THREAD || !p->drain_thread_started)\n        return PS3DK_PROF_EPERM;\n', '')],
                    'app', None),
}
if a.mutant:
    rel, edits, want_mode, want_san = MUTANTS[a.mutant]
    assert want_san is None or a.sanitize == want_san, f'mutant {a.mutant} is a {want_san} control'
    shutil.copytree(src, out / 'source')
    src = out / 'source'
    text = (src / rel).read_text()
    for old, new in edits:
        assert text.count(old) == 1, (a.mutant, old)
        text = text.replace(old, new)
    (src / rel).write_text(text)
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
for name in ['prof.h', 'prof_fifo.h']:
    shutil.copyfile(root / 'sdk/include/ps3dk' / name, inc / name)
shutil.copyfile(src / 'rsx/prof_gpu.h', inc / 'prof_gpu.h')
cc = os.environ.get('CC', 'cc')
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
           TSAN_OPTIONS='halt_on_error=1:exitcode=66')
commands = []


def run(name, argv, timeout=600):
    argv = [str(x) for x in argv]
    if a.sanitize == 'thread' and Path(argv[0]).parent == out:
        argv = ['setarch', 'x86_64', '-R', *argv]
    r = subprocess.run(argv, capture_output=True, timeout=timeout, env=env)
    (out / (name + '.stdout')).write_bytes(r.stdout)
    (out / (name + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name, argv=argv, rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2))
    return r


gcm = root / 'sdk/libgcm_cmd/src'
base = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-prio-ctor-dtor', '-pthread',
        '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DPS3DK_PROF_WITH_SPU',
        '-DPS3DK_PROF_WITH_PROFILE', '-DPS3DK_PROF_WITH_FIFO=1', '-I', inc.parent, '-I', src / 'core', '-I', src / 'pal',
        '-I', src / 'spu', '-I', src / 'rsx', '-I', src / 'fifo', '-I', gcm, '-fno-omit-frame-pointer']
if a.sanitize:
    base += ['-fsanitize=' + ('address,undefined' if a.sanitize == 'address' else 'thread'),
             '-fno-sanitize-recover=all', '-ftls-model=local-exec']
lib = [src / s for s in ['core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c',
                         'spu/psp_ppu.c', 'spu/psp_spu.c', 'spu/ext_runtime.c', 'spu/psp_elf.c', 'spu/psp_host_emul.c',
                         'rsx/gpu_runtime.c', 'rsx/prg.c', 'rsx/gpu_port_gcm.c', 'rsx/rsxcap.c',
                         'fifo/fifo_runtime.c', 'fifo/fifo_writer.c']]
objs = []
for f in lib + [here / 'fifo_test.c']:
    o = out / (f.stem + '.o')
    r = run(f'compile-{f.stem}', [cc, *base, '-c', f, '-o', o])
    assert r.returncode == 0, r.stderr.decode()
    objs.append(o)
digest = root / 'sdk/libps3dkprof/tools/archive_digest.py'
ident = ['--id', 'RSX-HOST', '--premises', '463', '--kinds', '1']
for name, argv in [('ar-members', ['ar', 'rcsD', out / 'members.a', *objs[:-1]]),
                   ('emit-identity', ['python3', '-I', '-B', digest, 'emit', out / 'members.a', *ident])]:
    r = run(name, argv)
    assert r.returncode == 0, r.stderr.decode()
(out / 'prof_identity.c').write_bytes(r.stdout)
for name, argv in [('compile-identity', [cc, *base, '-c', out / 'prof_identity.c', '-o', out / 'prof_identity.o']),
                   ('ar-final', ['ar', 'rcsD', out / 'libps3dkprof_ext_fifo.a', *objs[:-1], out / 'prof_identity.o']),
                   ('verify-identity', ['python3', '-I', '-B', digest, 'verify', out / 'libps3dkprof_ext_fifo.a', *ident]),
                   ('link', [cc, *base, objs[-1], out / 'libps3dkprof_ext_fifo.a', '-o', out / 'fifo_test'])]:
    r = run(name, argv)
    assert r.returncode == 0, (name, r.stdout.decode() + r.stderr.decode())

IOBASE = 0x100000
FREQ = None


def decode(path):
    b = path.read_bytes()
    assert b[:8] == b'PS3DKPRF', 'magic'
    major, minor, flags = struct.unpack_from('<HHI', b, 8)
    assert (major, minor) == (1, 0) and flags == 64 | 3, (major, minor, flags)  # bits 0, 1 and 6
    pos, seq, cap = 16, 0, dict(order=[], config=None, events=[], status=[], threads={}, torn=False, freq=None)
    while pos < len(b):
        if len(b) - pos < 20:
            cap['torn'] = True
            break
        typ, ln, s = struct.unpack_from('<IIQ', b, pos)
        assert s == seq and ln <= 1048576, (s, seq, ln)
        if len(b) - pos < 20 + ln:
            cap['torn'] = True
            break
        body = b[pos + 16:pos + 16 + ln]
        crc, = struct.unpack_from('<I', b, pos + 16 + ln)
        assert crc == zlib.crc32(b[pos:pos + 16 + ln]) & 0xFFFFFFFF, 'crc'
        cap['order'].append(typ)
        pos += 20 + ln
        seq += 1
        if typ == 2:
            cap['freq'] = struct.unpack_from('<Q', body, 12)[0]
        elif typ == 4:
            n, = struct.unpack_from('<I', body, 0)
            q = 4
            for _ in range(n):
                inc_, slot, osid, sl = struct.unpack_from('<IIQH', body, q)
                q += 18
                cap['threads'][body[q:q + sl].decode()] = inc_
                q += sl
        elif typ == 31:
            assert cap['config'] is None and not cap['events'] and not cap['status'] and ln == 64
            v = struct.unpack_from('<HHIIIIIIIQIIIIQ', body, 0)
            assert v[0] == 1 and v[1] == 1 and v[5] == 96 and v[13] == 0 and v[14] == 0, v
            assert 8 in cap['order'], 'CONFIG after the DOMAIN declaration'
            cap['config'] = dict(instance=v[2], domain=v[3], owner=v[4], ring=v[6], max=v[7], alloc=v[8], freq=v[9],
                                 begin=v[10], end=v[11], gen=v[12])
        elif typ == 32:
            assert cap['config'] is not None and not any(s['flags'] & 1 for s in cap['status'])
            schema, z0, inst, n, z1 = struct.unpack_from('<HHIII', body, 0)
            assert schema == 1 and z0 == z1 == 0 and inst == cap['config']['instance'] and n and ln == 16 + 96 * n
            for i in range(n):
                f = struct.unpack_from('<QQQQIHHIIQIIIIIIQQ', body, 16 + 96 * i)
                assert f[8] == 0 and f[15] == 0 and f[16] == 0 and f[17] == 0, f
                cap['events'].append(dict(seq=f[0], wrap=f[1], t0=f[2], t1=f[3], owner=f[4], kind=f[5], flags=f[6],
                                          fowner=f[7], frame=f[9], begin=f[10], tail=f[11], target=f[12], words=f[13],
                                          current=f[14]))
        elif typ == 33:
            assert cap['config'] is not None and ln == 80 and not any(s['flags'] & 1 for s in cap['status'])
            v = struct.unpack_from('<HHI9Q', body, 0)
            assert v[0] == 1 and v[2] == cap['config']['instance'] and v[1] < 16, v
            st = dict(zip(['flags', 'attempted', 'written', 'dropped', 'refused', 'last', 'started', 'completed',
                           'open', 'hw'], (v[1],) + v[3:]))
            assert st['written'] == len(cap['events']), (st, len(cap['events']))
            cap['status'].append(st)
    return cap


def check_events(c):
    """Producer-side invariants of every visible event (the reader re-checks them as MALFORMED rules)."""
    cfg = c['config']
    prev = None
    for e in c['events']:
        assert e['owner'] == cfg['owner'] and e['begin'] == cfg['begin'] and cfg['begin'] < e['tail'] <= cfg['end']
        tgt = {2: e['tail'], 3: e['tail'], 4: e['begin'], 5: e['begin']}.get(e['kind'], 0)
        assert e['target'] == tgt and e['current'] == (e['begin'] if e['kind'] == 6 else e['tail']), e
        assert e['flags'] & ~3 == 0 and (e['flags'] & 2 or e['t0'] <= e['t1']), e
        assert (e['flags'] & 1 and e['fowner'] == cfg['owner']) or (not e['flags'] & 1 and not e['fowner'] and not e['frame'])
        if prev:
            assert e['seq'] > prev['seq'] and (e['wrap'], e['kind']) > (prev['wrap'], prev['kind']), (prev, e)
            if e['seq'] == prev['seq'] + 1:
                assert (prev['kind'] == 6 and e['kind'] == 1 and e['wrap'] == prev['wrap'] + 1) or \
                       (e['wrap'] == prev['wrap'] and e['kind'] == prev['kind'] + 1), (prev, e)
            if not e['flags'] & 2:
                assert e['t0'] >= prev['t1'], (prev, e)
        prev = e


def wraps(c):
    out_ = {}
    for e in c['events']:
        out_.setdefault(e['wrap'], {})[e['kind']] = e
    return out_


def final(c):
    fin = [s for s in c['status'] if s['flags'] & 1]
    assert len(fin) == 1, c['status']
    return fin[0]


def reader_check(capf, expect_json):
    if not a.reader:
        return
    r = run('reader-' + capf.stem, [a.reader, 'fifo', '--json', capf])
    assert r.returncode in (0, 1, 4), (r.returncode, r.stderr.decode())
    v = json.loads(r.stdout)
    expect_json(v)


def go(mode, want=0):
    capf = out / (mode + '.prof')
    r = run(mode, [out / 'fifo_test', mode, capf])
    so = r.stdout.decode()
    assert r.returncode == want, (mode, r.returncode, so[-2000:], r.stderr.decode()[-3000:])
    assert b'runtime error' not in r.stderr and b'Sanitizer' not in r.stderr, r.stderr.decode()[-3000:]
    return capf, so


def kv(so, key):
    for line in so.splitlines():
        for tok in line.split():
            if tok.startswith(key + '='):
                return tok.split('=', 1)[1]
    raise KeyError(key)


def t_basic():
    capf, so = go('basic')
    c = decode(capf)
    cfg = c['config']
    assert cfg == dict(instance=1, domain=1026, owner=c['threads']['owner'], ring=1024, max=1048576, alloc=102400,
                       freq=c['freq'], begin=IOBASE + 16, end=IOBASE + 4092, gen=1), cfg
    check_events(c)
    assert [e['seq'] for e in c['events']] == list(range(1, 121))
    assert [e['kind'] for e in c['events']] == [1, 2, 3, 4, 5, 6] * 20
    assert all(e['flags'] == 1 and e['words'] == 64 for e in c['events'])
    fin = final(c)
    assert fin == dict(flags=1, attempted=120, written=120, dropped=0, refused=0, last=120, started=20, completed=20,
                       open=0, hw=fin['hw']) and 1 <= fin['hw'] <= 120, fin
    # the protocol's own stats window lies inside ENTER..EXIT and contains PHASE1_BEGIN..PHASE2_END
    w = wraps(c)
    inner = sum(x[5]['t1'] - x[2]['t0'] for x in w.values())
    outer = sum(x[6]['t1'] - x[1]['t0'] for x in w.values())
    count, ticks = int(kv(so, 'count')), int(kv(so, 'ticks'))
    assert count == fin['completed'] == 20 and inner <= ticks <= outer, (count, inner, ticks, outer)
    # frame tokens: frames begun at wraps 1, 5, 9, ... are frozen at each WRAP_ENTER
    frames = [w[k][1]['frame'] for k in sorted(w)]
    assert frames == sorted(frames) and len(set(frames)) == 5, frames
    assert all(len({e['frame'] for e in x.values()}) == 1 for x in w.values())
    reader_check(capf, lambda v: (v['coverage_status'] == 'FINAL' and len(v['wraps']) == 20 and
                                  all(x['phase1']['state'] == x['phase2']['state'] == x['outer']['state'] == 'COMPLETE'
                                      for x in v['wraps'])) or (_ for _ in ()).throw(AssertionError(v)))


def t_parked(mode):
    capf, so = go(mode)
    assert 'seen=1' in so, so
    c = decode(capf)
    check_events(c)
    fin = final(c)
    assert fin['attempted'] == fin['written'] == 24 and fin['completed'] == 4 and fin['dropped'] == 0, fin
    reader_check(capf, lambda v: len(v['wraps']) == 4 or (_ for _ in ()).throw(AssertionError(v)))


def t_stop(mode):
    capf, so = go(mode)
    assert kv(so, 'rc') == '-7' and kv(so, 'detach') == '-15', so
    c = decode(capf)
    check_events(c)
    fin = final(c)
    assert fin['flags'] == 1 | 2 | 8 and fin['started'] == 2 and fin['completed'] == 1 and fin['open'] == 2, fin
    last = c['events'][-1]
    assert last['wrap'] == 2 and last['kind'] == (4 if mode == 'stop2' else 2), last
    phase = 'phase2' if mode == 'stop2' else 'phase1'
    reader_check(capf, lambda v: (v['wraps'][-1][phase]['state'] == 'OPEN' and v['wraps'][-1]['outer']['state'] == 'OPEN'
                                  and v['status']['owner_unacknowledged']) or (_ for _ in ()).throw(AssertionError(v)))


def t_blocked():
    capf, so = go('blocked')
    assert 'owner_done=1 sink_blocked=1' in so, so
    c = decode(capf)
    check_events(c)
    fin = final(c)
    assert fin['flags'] == 1 and fin['dropped'] > 0 and fin['attempted'] == fin['written'] + fin['dropped'] == 1200, fin
    assert fin['hw'] == 16 and fin['completed'] == fin['started'] == 200, fin
    assert c['config']['ring'] == 16 and c['config']['alloc'] == 16 * 96 + 4096
    # a dropped attempt keeps its sequence: the visible sequence has gaps, never repeats
    seqs = [e['seq'] for e in c['events']]
    assert seqs == sorted(set(seqs)) and seqs[-1] <= 1200 and len(seqs) < 1200
    reader_check(capf, lambda v: any(x['phase1']['state'] == 'UNAVAILABLE' or x['outer']['state'] == 'UNAVAILABLE'
                                     for x in v['wraps']) or (_ for _ in ()).throw(AssertionError(v)))


def t_shortwrite():
    capf, so = go('shortwrite')
    assert kv(so, 'read_idx') == '0' and kv(so, 'writes_after_fail') == '0' and kv(so, 'rc') == '-10', so
    c = decode(capf)
    assert c['torn'] and not c['events'] and not c['status'] and c['config'] is not None, c


def t_lostfinal():
    capf, so = go('lostfinal')
    assert kv(so, 'writes_after_fail') == '0' and kv(so, 'rc') == '-10', so
    c = decode(capf)
    assert not any(s['flags'] & 1 for s in c['status']) and len(c['events']) == 120
    reader_check(capf, lambda v: v['coverage_status'].startswith('PROVISIONAL') or (_ for _ in ()).throw(AssertionError(v)))


def t_refusals():
    capf, so = go('refusals')
    c = decode(capf)
    check_events(c)
    fin = final(c)
    assert fin['attempted'] == fin['written'] == 6 and fin['completed'] == 1 and fin['refused'] == 0, fin


def t_app():
    capf, so = go('app')
    assert 'app attach rc=-1 instance=0' in so, so
    b = capf.read_bytes()
    assert struct.unpack_from('<I', b, 12)[0] == 64 | 3


def t_nogpu():
    go('nogpu')


def t_incb():
    capf, so = go('incb')
    assert kv(so, 'incb') == '-2', so


def t_nearmax():
    capf, so = go('nearmax')
    c = decode(capf)
    fin = final(c)
    assert [e['seq'] for e in c['events']] == [2 ** 63 - 3, 2 ** 63 - 2, 2 ** 63 - 1]
    assert fin['flags'] == 1 | 4 and fin['attempted'] == 2 ** 63 - 1 and fin['refused'] == 15, fin
    reader_check(capf, lambda v: v['status']['saturated'] or (_ for _ in ()).throw(AssertionError(v)))


def t_recursion():
    capf, so = go('recursion')
    assert kv(so, 'suppressed') == '6' and kv(so, 'count') == '21', so
    c = decode(capf)
    check_events(c)
    assert final(c)['completed'] == 20 and len(c['events']) == 120


def t_attach(mode):
    # attachrace: held after validation, before the instance claim; attachseal: claimed and installed, before the
    # publication.  Shutdown completes meanwhile; the attach must refuse, leave no observer and no live allocation.
    capf, so = go(mode)
    assert 'attach rc=-15 instance=0 observer=0 block=0 state=5' in so, so
    c = decode(capf)
    assert c['config'] is None and not c['events'] and not c['status'], c


def t_race(mode='race'):
    capf, so = go(mode)
    c = decode(capf)
    check_events(c)
    fin = final(c)
    assert fin['attempted'] == 120000 == fin['written'] + fin['dropped'] and fin['completed'] == 20000, fin
    print('%s written=%d dropped=%d hw=%d statuses=%d' % (mode, fin['written'], fin['dropped'], fin['hw'], len(c['status'])))
    if mode == 'pace':
        assert fin['hw'] < 4096, fin   # never full: the publication edge alone orders the slots


if a.mutant:
    want_mode = MUTANTS[a.mutant][2]
    r = run('mutant-' + want_mode, [out / 'fifo_test', want_mode, out / (want_mode + '-mutant.prof')])
    se = r.stderr.decode()
    if MUTANTS[a.mutant][3] == 'thread':
        hit = r.returncode == 66 and 'ThreadSanitizer: data race' in se
    else:
        hit = r.returncode == 1 and 'FAIL' in se
    print('mutant %s control=%s detected=%s rc=%d' % (a.mutant, want_mode, hit, r.returncode), flush=True)
    raise SystemExit(0 if hit else 1)

results = []
cases = [('basic', t_basic), ('parked1', lambda: t_parked('parked1')), ('parked2', lambda: t_parked('parked2')),
         ('gpuzone', lambda: t_parked('gpuzone')), ('stop1', lambda: t_stop('stop1')),
         ('stop2', lambda: t_stop('stop2')), ('blocked', t_blocked), ('shortwrite', t_shortwrite),
         ('lostfinal', t_lostfinal), ('refusals', t_refusals), ('app', t_app), ('nogpu', t_nogpu), ('incb', t_incb),
         ('nearmax', t_nearmax), ('attachrace', lambda: t_attach('attachrace')),
         ('attachseal', lambda: t_attach('attachseal')), ('recursion', t_recursion), ('race', t_race), ('pace', lambda: t_race('pace'))]
if a.case:
    assert set(a.case) <= {n for n, _ in cases}, a.case
    cases = [(n, f) for n, f in cases if n in a.case]
for name, fn in cases:
    fn()
    results.append(name)
    print(name, 'PASS', flush=True)
print('cases=%d passed=%d sanitize=%s reader=%s' % (len(cases), len(results), a.sanitize, bool(a.reader)), flush=True)
raise SystemExit(0 if len(results) == len(cases) else 1)
