#!/usr/bin/env python3
"""Function-path runtime (libps3dkprof func variant) on the host: build the real core and func runtime without
instrumentation, the workload with -finstrument-functions, run every control, and decode the capture bytes here
(a minimal strict decoder of chunk types 28-30, not the reader).  Contract: docs/design/ps3dk-prof-func-paths.md."""
import argparse, json, os, shutil, struct, subprocess, zlib
from collections import defaultdict
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--sanitize', choices=['address', 'thread'])
ap.add_argument('--mutant', choices=['relaxed-pub', 'relaxed-ack', 'ignore-ack'],
                help='break the handoff in a copy of the sources; the race control must then fail under TSan')
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
here = Path(__file__).resolve().parent
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
src = root / 'sdk/libps3dkprof/src'
if a.mutant:
    assert a.sanitize == 'thread', 'mutants are TSan controls'
    shutil.copytree(src, out / 'source')
    src = out / 'source'
    old, new, rel = {'relaxed-pub': ('ST_REL(&a->pub, e);', 'ST_RLX(&a->pub, e);', 'func/func_hooks.c'),
                     'relaxed-ack': ('ST_REL(&a->ack, pub);', 'ST_RLX(&a->ack, pub);', 'func/func_runtime.c'),
                     'ignore-ack': ('if (LD_ACQ(&a->ack) != a->last_pub) {', 'if (0) {', 'func/func_hooks.c')}[a.mutant]
    text = (src / rel).read_text()
    assert text.count(old) == 1, old
    (src / rel).write_text(text.replace(old, new))
inc = out / 'include/ps3dk'
inc.mkdir(parents=True)
for name in ['prof.h', 'prof_func.h']:
    shutil.copyfile(root / 'sdk/include/ps3dk' / name, inc / name)
cc = os.environ.get('CC', 'cc')
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
           TSAN_OPTIONS='halt_on_error=1:exitcode=66')
commands = []


def run(name, argv, timeout=300):
    argv = [str(x) for x in argv]
    if a.sanitize == 'thread' and Path(argv[0]).parent == out:
        argv = ['setarch', 'x86_64', '-R', *argv]
    r = subprocess.run(argv, capture_output=True, timeout=timeout, env=env)
    (out / (name + '.stdout')).write_bytes(r.stdout)
    (out / (name + '.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name, argv=argv, rc=r.returncode))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2))
    return r


base = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-prio-ctor-dtor', '-pthread',
        '-DPS3DK_PROF=1', '-DPS3DK_PROF_PAL_HOST', '-DPS3DK_PROF_TEST_HOOKS', '-DPS3DK_PROF_WITH_FUNC=1',
        '-I', inc.parent, '-I', src / 'core', '-I', src / 'func', '-fno-omit-frame-pointer']
if a.sanitize:
    base += ['-fsanitize=' + ('address,undefined' if a.sanitize == 'address' else 'thread'),
             '-fno-sanitize-recover=all', '-ftls-model=local-exec']
lib = [src / s for s in ['core/prof_core.c', 'core/prof_drain.c', 'core/prof_version.c', 'pal/pal_host.c',
                         'func/func_runtime.c', 'func/func_hooks.c']]
objs = []
for i, f in enumerate(lib + [here / 'func_test.c']):
    o = out / f'o{i}.o'
    r = run(f'compile-{i}', [cc, *base, '-fno-instrument-functions', '-c', f, '-o', o])
    assert r.returncode == 0, r.stderr.decode()
    objs.append(o)
o = out / 'work.o'
# x86-64 GCC tail-calls the exit hook of a void function after `leave` (its frame is then the caller's), which the
# frame check correctly reports as a stack loss; the PPU ELFv1 hook call is bl+nop (no sibling call).  The host
# workload therefore disables sibling calls, as an LTO build that makes the hook local must.
r = run('compile-work', [cc, *base, '-finstrument-functions', '-fno-optimize-sibling-calls', '-c',
                         here / 'func_work.c', '-o', o])
assert r.returncode == 0, r.stderr.decode()
r = run('link', [cc, *base, *objs, o, '-o', out / 'func_test'])
assert r.returncode == 0, r.stderr.decode()
# The library is never instrumented: no hook call may appear in any library object.
for obj in objs[:-1]:
    r = run('nm-' + obj.stem, ['nm', '-u', obj])
    assert r.returncode == 0 and b'__cyg_profile_func' not in r.stdout, obj

FLAG_NAMES = dict(DATA=1, LAGGING=2, FINAL=4, INCOMPLETE=8, RESET=16, UNKNOWN=32, DEPTH=64, LOST=128, SAT=256,
                  OPEN=512, CLOCK=1024)
F = type('F', (), FLAG_NAMES)
ROOT, UNKNOWN, LOSTP = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD
CNT = ['depth_limit', 'lost', 'unbalanced', 'orphan', 'deferred', 'reentry', 'clock']


def decode(path):
    b = path.read_bytes()
    assert b[:8] == b'PS3DKPRF', 'magic'
    major, minor, flags = struct.unpack_from('<HHI', b, 8)
    assert (major, minor) == (1, 0) and flags == 32, (major, minor, flags)
    pos, seq, chunks = 16, 0, []
    while pos < len(b):
        typ, ln, s = struct.unpack_from('<IIQ', b, pos)
        assert s == seq and ln <= 1048576, (s, seq, ln)
        body = b[pos + 16:pos + 16 + ln]
        assert len(body) == ln, 'torn'
        crc, = struct.unpack_from('<I', b, pos + 16 + ln)
        assert crc == zlib.crc32(b[pos:pos + 16 + ln]) & 0xFFFFFFFF, 'crc'
        chunks.append((typ, body))
        pos += 20 + ln
        seq += 1
    cap = dict(config=None, trees=[], status=[], threads={}, order=[])
    for typ, body in chunks:
        cap['order'].append(typ)
        if typ == 4:
            n, = struct.unpack_from('<I', body, 0)
            q = 4
            for _ in range(n):
                inc_, slot, osid, sl = struct.unpack_from('<IIQH', body, q)
                q += 18
                cap['threads'][inc_] = body[q:q + sl].decode()
                q += sl
        elif typ == 28:
            assert cap['config'] is None and len(body) == 48 and not cap['trees'] and not cap['status']
            v = struct.unpack_from('<HHIIIIIIIQQ', body, 0)
            assert v[0] == 1 and v[1] == 1 and v[8] == 0 and v[6] >= 2 * v[4] and v[9] > 0, v
            cap['config'] = dict(ptr=v[2], actors=v[3], cap=v[4], depth=v[5], hs=v[6], probe=v[7], freq=v[9],
                                 bytes=v[10])
        elif typ == 29:
            assert cap['config'] is not None
            h = struct.unpack_from('<HHIQQQQQIIII', body, 0)
            schema, fl, inc_, interval, ans, req, start, cut, nd, nr, opn, sup = h
            cnt = struct.unpack_from('<8Q', body, 64)
            assert schema == 1 and cnt[7] == 0 and fl < 2048 and len(body) == 128 + 16 * nd + 40 * nr, h
            defs, rows, q = [], [], 128
            for _ in range(nd):
                defs.append(struct.unpack_from('<IIQ', body, q)); q += 16
            for _ in range(nr):
                node, rf, r0, op, r1, calls, incl, child = struct.unpack_from('<IHHIIQQQ', body, q); q += 40
                assert r0 == 0 and r1 == 0 and rf < 32 and child <= incl, (node, rf, child, incl)
                rows.append(dict(node=node, flags=rf, open=op, calls=calls, incl=incl, child=child))
            if not fl & F.DATA:
                assert nd == nr == interval == ans == start == cut == opn == sup == 0 and not any(cnt), h
            cap['trees'].append(dict(flags=fl, inc=inc_, interval=interval, ans=ans, req=req, start=start, cut=cut,
                                     defs=defs, rows=rows, open=opn, sup=sup, cnt=dict(zip(CNT, cnt))))
        elif typ == 30:
            assert len(body) == 32
            s0, fl, r, req, un, na = struct.unpack_from('<HHIQQQ', body, 0)
            assert s0 == 1 and r == 0 and fl < 4
            cap['status'].append(dict(flags=fl, req=req, unreg=un, noactor=na))
        else:
            assert typ <= 7, typ
    assert cap['status'] and cap['status'][-1]['flags'] & 1 and sum(s['flags'] & 1 for s in cap['status']) == 1
    # Per incarnation: definitions before use, consecutive intervals, contiguous ticks, at most one terminal chunk.
    per = defaultdict(lambda: dict(nodes={}, last=0, cut=None, term=0, totals=defaultdict(lambda: [0, 0, 0]),
                                   cnt=defaultdict(int), chunks=[]))
    for t in cap['trees']:
        st = per[t['inc']]
        assert not st['term'], 'chunk after terminal'
        st['chunks'].append(t)
        if t['flags'] & (F.FINAL | F.INCOMPLETE):
            st['term'] = 1
        if not t['flags'] & F.DATA:
            continue
        assert t['interval'] == st['last'] + 1, (t['interval'], st['last'])
        if st['cut'] is not None:
            assert t['start'] == st['cut'], (t['start'], st['cut'])
        assert t['cut'] >= t['start']
        st['last'], st['cut'] = t['interval'], t['cut']
        for node, parent, fn in t['defs']:
            assert node not in st['nodes'] and node == len(st['nodes']), node
            if node == 0:
                assert parent == LOSTP and fn == 0
            else:
                assert parent in (ROOT, UNKNOWN) or parent in st['nodes'] and parent != 0, parent
            st['nodes'][node] = (parent, fn)
        for row in t['rows']:
            assert row['node'] in st['nodes'], 'row before definition'
            assert bool(row['flags'] & 4) == (row['node'] == 0)
            tot = st['totals'][row['node']]
            tot[0] += row['calls']; tot[1] += row['incl']; tot[2] += row['child']
        for k, v in t['cnt'].items():
            st['cnt'][k] += v
    cap['per'] = per
    return cap


def names(stdout):
    return {int(l.split()[2]): l.split()[1] for l in stdout.decode().splitlines() if l.startswith('sym ')}


def path_of(st, sym, node):
    parts = []
    while True:
        if node == 0:
            return ('LOST',) + tuple(parts)
        parent, fn = st['nodes'][node]
        parts.insert(0, sym.get(fn, hex(fn)))
        if parent == ROOT:
            return tuple(parts)
        if parent == UNKNOWN:
            return ('?',) + tuple(parts)
        node = parent


def totals(cap, sym, inc_):
    st = cap['per'][inc_]
    return {path_of(st, sym, n): tuple(v) for n, v in st['totals'].items() if any(v)}


def interval_rows(cap, sym, chunk, inc_):
    st = cap['per'][inc_]
    return {path_of(st, sym, r['node']): r for r in chunk['rows']}


def inc_named(cap, name):
    return [i for i, n in cap['threads'].items() if n == name]


def go(mode, want=0):
    capf = out / (mode + '.prof')
    r = run(mode, [out / 'func_test', mode, capf])
    assert r.returncode == want, (mode, r.returncode, r.stderr.decode()[-3000:])
    assert b'runtime error' not in r.stderr and b'Sanitizer' not in r.stderr, r.stderr.decode()[-3000:]
    return decode(capf), names(r.stdout), r.stdout.decode()


results = []


def case(name, fn):
    fn()
    results.append(name)
    print(name, 'PASS', flush=True)


TREE = {('tree_a',): (2, 252, 240), ('tree_a', 'tree_b'): (4, 40, 0), ('tree_a', 'tree_c'): (2, 200, 0),
        ('rec',): (1, 3, 2), ('rec', 'rec'): (1, 2, 1), ('rec', 'rec', 'rec'): (1, 1, 0)}


def t_tree():
    c1, s1, _ = go('tree1')
    m1 = inc_named(c1, 'main')[0]
    assert totals(c1, s1, m1) == TREE, totals(c1, s1, m1)
    c3, s3, _ = go('tree3')
    m3 = inc_named(c3, 'main')[0]
    assert totals(c3, s3, m3) == TREE, totals(c3, s3, m3)
    data = [t for t in c3['per'][m3]['chunks'] if t['flags'] & F.DATA]
    assert [t['interval'] for t in data] == [1, 2, 3] and data[-1]['flags'] & F.FINAL
    assert [t['ans'] for t in data] == [1, 2, 0], [t['ans'] for t in data]
    i1, i2, i3 = (interval_rows(c3, s3, t, m3) for t in data)
    assert (i1[('tree_a',)]['calls'], i1[('tree_a',)]['incl'], i1[('tree_a',)]['child']) == (1, 126, 120)
    assert ('rec',) in i2 and ('tree_a',) not in i2 and i3[('tree_a',)]['calls'] == 1
    assert all(not r['flags'] & 1 for t in data for r in t['rows']), 'no open activations at these cuts'
    # incl == self + child for each row and interval: child of a parent equals the incl of its only-child sets.
    for rows in (i1, i3):
        assert rows[('tree_a',)]['child'] == rows[('tree_a', 'tree_b')]['incl'] + rows[('tree_a', 'tree_c')]['incl']


def t_depth():
    c, s, _ = go('depth')
    m = inc_named(c, 'main')[0]
    st, tot = c['per'][m], totals(c, s, inc_named(c, 'main')[0])
    for k in range(1, 9):
        p = ('deep',) * k
        assert tot[p] == (1, 13 - k, 12 - k if k < 8 else 0), (p, tot[p])
    assert ('deep',) * 9 not in tot
    assert st['cnt']['depth_limit'] == 8 and st['cnt']['unbalanced'] == 1 and st['cnt']['orphan'] == 0, st['cnt']
    fin = st['chunks'][-1]
    assert fin['flags'] & F.DEPTH and fin['flags'] & F.RESET and fin['flags'] & F.FINAL
    rows = interval_rows(c, s, fin, m)
    assert rows[('deep',) * 8]['flags'] & 2, 'SELF_UNTIMED on the deepest retained level'
    for k in range(1, 9):
        r = rows[('deep_jump',) * k]
        assert r['flags'] & 16 and r['calls'] == 1 and r['incl'] == 0, r   # TRUNCATED, never guessed


def t_overflow():
    c, s, _ = go('overflow')
    m = inc_named(c, 'main')[0]
    st, tot = c['per'][m], totals(c, s, m)
    assert c['config']['cap'] == 4 and len(st['nodes']) == 4
    assert tot == {('ov_root',): (1, 23, 22), ('ov_root', 'ov_f1'): (1, 1, 0), ('ov_root', 'ov_f2'): (1, 2, 0),
                   ('LOST',): (4, 19, 0)}, tot
    assert st['nodes'][0] == (LOSTP, 0) and all(p != 0 for p, _ in st['nodes'].values()), 'LOST is never a parent'
    assert st['cnt']['lost'] == 4 and st['chunks'][-1]['flags'] & F.LOST


def t_longjmp():
    c, s, _ = go('longjmp')
    m = inc_named(c, 'main')[0]
    st, tot = c['per'][m], totals(c, s, m)
    assert st['cnt']['unbalanced'] == 2 and st['cnt']['orphan'] == 1, st['cnt']
    assert tot[('?', 'lj_after')] == (1, 4, 0) and tot[('?', 'post_reset')] == (1, 2, 0), tot
    for p in [('lj_outer',), ('lj_outer', 'lj_mid'), ('lj_outer', 'lj_mid', 'lj_deep'), ('?', 'ra'), ('?', 'ra', 'ra'),
              ('?', 'ra', 'ra', 'ra')]:
        assert tot[p] == (1, 0, 0), (p, tot.get(p))
    fin = st['chunks'][-1]
    assert fin['flags'] & F.RESET and fin['flags'] & F.UNKNOWN
    assert all(r['flags'] & 16 for p, r in interval_rows(c, s, fin, m).items() if p[-1] in ('lj_outer', 'lj_mid', 'lj_deep', 'ra'))


def t_span():
    c, s, _ = go('span')
    m = inc_named(c, 'main')[0]
    st, tot = c['per'][m], totals(c, s, m)
    assert tot == {('sp_outer',): (1, 27, 17), ('sp_outer', 'sp_inner'): (1, 17, 2),
                   ('sp_outer', 'sp_inner', 'sp_leaf'): (2, 2, 0)}, tot
    data = [t for t in st['chunks'] if t['flags'] & F.DATA]
    assert [t['interval'] for t in data] == [1, 2, 3, 4]
    i2, i3, i4 = (interval_rows(c, s, t, m) for t in data[1:])
    o, n = ('sp_outer',), ('sp_outer', 'sp_inner')
    assert data[1]['flags'] & F.OPEN and data[1]['open'] == 2 and data[1]['cut'] == 1010
    assert (i2[o]['calls'], i2[o]['incl'], i2[o]['child'], i2[o]['open'], i2[o]['flags'] & 1) == (1, 10, 5, 1, 1)
    assert (i2[n]['calls'], i2[n]['incl'], i2[n]['child'], i2[n]['open']) == (1, 5, 0, 1)
    assert (i3[o]['calls'], i3[o]['incl'], i3[o]['child']) == (0, 6, 6) and (i3[n]['incl'], i3[n]['child']) == (6, 1)
    assert (i4[o]['incl'], i4[o]['child'], i4[n]['incl'], i4[n]['child']) == (11, 6, 6, 1)
    for rows in (i2, i3, i4):   # child of each parent == incl of its child, per interval
        assert rows[o]['child'] == rows[n]['incl']


def t_reentry():
    c, s, _ = go('reentry')
    m = inc_named(c, 'main')[0]
    st, tot = c['per'][m], totals(c, s, m)
    # the refused call ran inside the first leaf's entry hook: its tick is in that leaf's elapsed time (hook cost is
    # never subtracted), and it has no node of its own
    assert st['cnt']['reentry'] == 2 and tot == {('leaf',): (2, 1, 0)}, (st['cnt'], tot)


def t_reg():
    c, s, _ = go('reg')
    m = inc_named(c, 'main')[0]
    st, tot = c['per'][m], totals(c, s, m)
    assert tot == {('reg_inner',): (1, 6, 0)} and st['cnt']['orphan'] == 1, (tot, st['cnt'])
    assert sum(x['unreg'] for x in c['status']) == 0


def t_unreg():
    c, s, _ = go('unreg')
    assert sum(x['unreg'] for x in c['status']) == 5 and sum(x['noactor'] for x in c['status']) == 3, c['status']
    assert not inc_named(c, 'noactor') or all(t['inc'] not in inc_named(c, 'noactor') for t in c['trees'])
    c, s, _ = go('auto')
    w = inc_named(c, 'auto')[0]
    assert totals(c, s, w) == {('v_fn',): (2, 0, 0)} and sum(x['unreg'] for x in c['status']) == 0


def t_threads():
    c, s, _ = go('threads')
    w = inc_named(c, 'lagw')[0]
    ch = c['per'][w]['chunks']
    kinds = [(t['flags'] & (F.DATA | F.LAGGING | F.FINAL), t['ans'], t['req'], t['cut']) for t in ch]
    assert kinds == [(F.LAGGING, 0, 1, 0), (F.LAGGING, 0, 2, 0), (F.DATA | F.LAGGING, 2, 3, 4000),
                     (F.DATA, 4, 4, 7000), (F.DATA | F.FINAL, 0, 5, 8500)], kinds
    assert ch[2]['start'] == 1000, ch[2]['start']   # interval 1 starts at registration
    assert ch[3]['cnt']['deferred'] == 1 and ch[2]['cnt']['deferred'] == 0
    # entered at 4000, held in the hook while the clock moved to 5000, then 3 ticks: elapsed wall time is 1003
    assert interval_rows(c, s, ch[3], w)[('w1',)]['incl'] == 1003 and interval_rows(c, s, ch[4], w)[('w1',)]['calls'] == 1
    assert all(t['cut'] not in (2000, 3000, 5000, 6000, 8000, 9000) for t in ch), 'cuts are the owner ticks'


def t_stop():
    c, s, _ = go('stop')
    r, sp, idle = (inc_named(c, n)[0] for n in ('retire', 'spin', 'idle'))
    fin = c['per'][r]['chunks'][-1]
    rows = interval_rows(c, s, fin, r)
    assert fin['flags'] & F.FINAL and fin['flags'] & F.OPEN and fin['open'] == 1
    assert (rows[('r_outer',)]['incl'], rows[('r_outer',)]['child'], rows[('r_outer',)]['open']) == (3, 2, 1)
    assert totals(c, s, r)[('r_outer', 'r_leaf')] == (1, 2, 0)
    assert c['per'][sp]['chunks'][-1]['flags'] & F.FINAL and totals(c, s, sp)[('s_spin',)][0] > 0
    last = c['per'][idle]['chunks'][-1]
    assert last['flags'] == F.LAGGING | F.INCOMPLETE, last


def t_race():
    c, s, so = go('race')
    w = inc_named(c, 'owner')[0]
    st, tot = c['per'][w], totals(c, s, w)
    assert tot[('rp',)][0] == 200000 and tot[('rp', 'rq')][0] == 400000, tot
    data = [t for t in st['chunks'] if t['flags'] & F.DATA]
    assert len(data) >= 3, len(data)
    for t in data:
        rows = interval_rows(c, s, t, w)
        if ('rp',) in rows and ('rp', 'rq') in rows:
            assert rows[('rp',)]['child'] == rows[('rp', 'rq')]['incl'], (t['interval'], rows)
            assert abs(rows[('rp', 'rq')]['calls'] - 2 * rows[('rp',)]['calls']) <= 2
        assert all(r['child'] == 0 for p, r in rows.items() if p == ('rp', 'rq'))
    print('race data_chunks=%d deferred=%d %s' % (len(data), st['cnt']['deferred'], so.split('drains=')[1].split()[0]))


def t_cost():
    c, s, so = go('cost')
    line = [l for l in so.splitlines() if l.startswith('cost ')][0]
    print(line)
    (out / 'cost.txt').write_text(line + '\n')


if a.mutant:
    r = run('race-mutant', [out / 'func_test', 'race', out / 'race-mutant.prof'])
    hit = r.returncode == 66 and b'ThreadSanitizer: data race' in r.stderr
    print('mutant %s detected=%s rc=%d' % (a.mutant, hit, r.returncode), flush=True)
    raise SystemExit(0 if hit else 1)
for name, fn in [('tree', t_tree), ('depth', t_depth), ('overflow', t_overflow), ('longjmp', t_longjmp),
                 ('span', t_span), ('reentry', t_reentry), ('reg', t_reg), ('unreg', t_unreg),
                 ('threads', t_threads), ('stop', t_stop), ('race', t_race), ('cost', t_cost)]:
    case(name, fn)
print('cases=%d passed=%d sanitize=%s' % (12, len(results), a.sanitize), flush=True)
raise SystemExit(0 if len(results) == 12 else 1)
