#!/usr/bin/env python3
"""Host test of the rsx-frame sample's checker (tools/check_capture.py), no capture needed.

Builds the reader JSON a matching capture produces from expected/draws.tsv (reader schema 1 field names) and the
shader-hash JSON of the five programs, requires the checker to accept them, then breaks one field at a time and
requires the checker to refuse each with a difference naming that field. Exit 0 only when every case behaved."""
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'tools/check_capture.py'
spec = importlib.util.spec_from_file_location('check_capture', TOOL)
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

HASHES = [{'file': f'build/shaders/{s}.{ext}', 'profile': ext[:2], 'hash': f'{0x1111 * (i + 1):016x}'}
          for i, (s, ext) in enumerate([('vp_offset', 'vpo'), ('vp_scale', 'vpo'), ('fp_color', 'fpo'),
                                        ('fp_tex', 'fpo'), ('fp_texmod', 'fpo')])]


def matching(meta, rows, state_rows=(), const_rows=()):
    """The reader JSON of a capture equal to the tables."""
    stems = cc.hashes_by_stem(HASHES)
    programs, index_of = [], {}
    for r in rows:
        for col in ('vp', 'fp'):
            s = r[col]
            if s != '-' and s not in index_of:
                index_of[s] = len(programs) + 1
                profile, h = stems[s]
                programs.append({'index': index_of[s], 'profile': profile, 'hash': h, 'id': f'{profile}:{h}',
                                 'unterminated': False, 'unreadable': False, 'not_revalidated': False})
    pass_names = sorted({r['pass'] for r in rows if r['pass'] != '-'})
    draws = []
    for r in rows:
        t, loc = r['index'].split('/') if r['index'] != '-' else (None, None)
        draws.append({
            'ordinal': int(r['ordinal']), 'kind': r['kind'], 'primitive': r['primitive'], 'count': int(r['count']),
            'first': int(r['first']), 'index': {'type': t, 'location': loc} if t else None,
            'label': None if r['label'] == '-' else r['label'], 'tag': int(r['tag'], 0),
            'vp': index_of.get(r['vp']), 'fp': index_of.get(r['fp']),
            'pass': pass_names.index(r['pass']) if r['pass'] != '-' else None,
            'timing': {'state': r['timing']}, 'hooked': True, 'decoded': True, 'mismatch': False,
            'state_unknown': False,
            'state': {'render': {}, 'surfaces': {}, 'index': {}, 'textures': [{} for _ in range(16)],
                      'attributes': [{} for _ in range(16)]}})
    for ordinal, profile, name, value in const_rows:
        d = draws[ordinal]
        d.setdefault('constants', {'vp': [], 'fp': [], 'vp_changed': [], 'fp_list': []})
        d['constants'][profile].append({'name': name, 'values': [value]})
    for d in draws:
        d.setdefault('constants', {'vp': [], 'fp': [], 'vp_changed': [], 'fp_list': []})
    for ordinal, table, field, value in state_rows:
        for d in draws:
            if ordinal is None or d['ordinal'] == ordinal:
                section, _, slot = table.partition('.')
                (d['state'][section][int(slot)] if slot else d['state'][section])[field] = value
    return {'schema': 1, 'capability': True, 'availability': 'available', 'decoder': 5, 'captures': [{
        'id': 1, 'name': meta['name'], 'source': meta['source'], 'status': meta['status'],
        'timing_mode': meta['timing_mode'], 'text_replaced': 0,
        'totals': {'draws': len(draws), 'timed': len(draws), 'untimed': 0, 'missing': 0, 'mismatched': 0},
        'programs': programs, 'passes': [{'name': n} for n in pass_names], 'draws': draws,
        'instance_runs': [{'first': int(f), 'length': int(n), 'vp_uniforms': [u for u in names.split(',') if u]}
                          for f, n, names in (x.split(':') for x in meta.get('instance_runs', '').split())]}]}


def run(doc, work, name):
    rj, hj = work / f'{name}.json', work / 'hashes.json'
    rj.write_text(json.dumps(doc))
    hj.write_text(json.dumps(HASHES))
    r = subprocess.run([sys.executable, '-I', str(TOOL), '--reader-json', str(rj), '--hashes-json', str(hj)],
                       capture_output=True, timeout=60)
    return r.returncode, r.stdout.decode()


def main():
    meta, rows = cc.load_table(TOOL.parent.parent / 'expected/draws.tsv')
    assert len(rows) == int(meta['draws']) == 19, (len(rows), meta)
    state_rows = cc.load_state(TOOL.parent.parent / 'expected/state.tsv')
    assert len(state_rows) > 100, len(state_rows)
    const_rows = cc.load_constants(TOOL.parent.parent / 'expected/constants.tsv')
    assert len(const_rows) == 18, len(const_rows)
    base = matching(meta, rows, state_rows, const_rows)
    # the BOTH capture: two views of the same draws (the second's ordinals after the first's), every draw paired,
    # every program's bytes complete
    one = base['captures'][0]
    for half, mode, off in (('first', 'pipelined', 0), ('second', 'isolated', 19)):
        v = copy.deepcopy(one)
        v.update(name='matrix-both', half=half, timing_mode=mode,
                 program_bytes=[{'index': p['index'], 'bytes': 64, 'complete': True} for p in one['programs']],
                 pairs=[{'first': i, 'second': i} for i in range(19)] if half == 'second' else [])
        for dr in v['draws']:
            dr['ordinal'] += off
        base['captures'].append(v)

    def d(doc, i):
        return doc['captures'][0]['draws'][i]

    def prog(doc, i):
        return doc['captures'][0]['programs'][i]

    cases = {
        'label': (lambda x: d(x, 3).__setitem__('label', 'frame/matrix/other'), 'draw 3 label'),
        'kind': (lambda x: d(x, 7).__setitem__('kind', 'ARRAY'), 'draw 7 kind'),
        'count': (lambda x: d(x, 1).__setitem__('count', 3), 'draw 1 count'),
        'index': (lambda x: d(x, 7).__setitem__('index', {'type': 'u32', 'location': 'local'}), 'draw 7 index'),
        'tag': (lambda x: d(x, 9).__setitem__('tag', 0x301), 'draw 9 tag'),
        'pass': (lambda x: d(x, 7).__setitem__('pass', 0), 'draw 7 pass'),
        'timing': (lambda x: d(x, 12).__setitem__('timing', {'state': 'untimed'}), 'draw 12 timing'),
        'fp swapped': (lambda x: d(x, 2).__setitem__('fp', d(x, 3)['fp']), 'draw 2 fp'),
        'vp hash': (lambda x: prog(x, 0).__setitem__('hash', 'deadbeefdeadbeef'), 'draw 1 vp'),
        'not revalidated': (lambda x: prog(x, 1).__setitem__('not_revalidated', True), 'not_revalidated'),
        'mismatch': (lambda x: d(x, 5).__setitem__('mismatch', True), 'draw 5 mismatch'),
        'hooked': (lambda x: d(x, 0).__setitem__('hooked', False), 'draw 0 hooked'),
        'status': (lambda x: x['captures'][0].__setitem__('status', 'PENDING TIMING (1 AUTO records not yet present)'),
                   'capture status'),
        'extra draw': (lambda x: x['captures'][0]['draws'].append(dict(d(x, 18), ordinal=19)), 'draw 19'),
        'missing draw': (lambda x: x['captures'][0]['draws'].pop(), 'draw 18: missing'),
        'clear with fp': (lambda x: d(x, 0).__setitem__('fp', 1), 'draw 0 fp'),
        'no capture': (lambda x: x['captures'][0].__setitem__('name', 'other'), 'no capture named'),
        'depth test': (lambda x: d(x, 2)['state']['render'].__setitem__('depth.test', False), 'draw 2 render depth.test'),
        'texture format': (lambda x: d(x, 3)['state']['textures'][0].__setitem__('format', 'A8R8G8B8'),
                           'draw 3 textures.0 format'),
        'attribute stride': (lambda x: d(x, 1)['state']['attributes'][3].__setitem__('stride', 16),
                             'draw 1 attributes.3 stride'),
        'surface unknown': (lambda x: d(x, 11)['state']['surfaces'].__setitem__('zeta_format', None),
                            'draw 11 surfaces zeta_format'),
        'index type': (lambda x: d(x, 7)['state']['index'].__setitem__('type', 'u32'), 'draw 7 index type'),
        'no state': (lambda x: d(x, 9).pop('state'), 'draw 9: no register state'),
        'decoder 1': (lambda x: x.__setitem__('decoder', 1), 'decoder: 1'),
        'both half missing': (lambda x: x['captures'].pop(), "halves ['first']"),
        'both unpaired': (lambda x: x['captures'][2]['pairs'][3].__setitem__('second', None), '1 unpaired'),
        'both bytes incomplete': (lambda x: x['captures'][1]['program_bytes'][2].__setitem__('complete', False),
                                  'program 3 bytes'),
        'both isolated draw kind': (lambda x: x['captures'][2]['draws'][7].__setitem__('kind', 'ARRAY'),
                                    'matrix-both isolated: draw 7 kind'),
        'constant value': (lambda x: d(x, 10)['constants']['vp'][0].__setitem__('values', [[-0.36, -0.8, 0, 0]]),
                           'draw 10 vp.offset'),
        'uniform missing': (lambda x: d(x, 4)['constants'].__setitem__('vp', []), 'draw 4 vp.xform: no such uniform'),
        'no constants': (lambda x: d(x, 7).pop('constants'), 'draw 7: no constants'),
        'run shorter': (lambda x: x['captures'][0]['instance_runs'][0].__setitem__('length', 7), 'instance runs'),
        'extra run': (lambda x: x['captures'][0]['instance_runs'].append({'first': 1, 'length': 2, 'vp_uniforms': []}),
                      'instance runs'),
    }
    with tempfile.TemporaryDirectory() as t:
        work = Path(t)
        rc, out = run(base, work, 'match')
        assert rc == 0 and 'MATCHES' in out, out
        for name, (mutate, needle) in cases.items():
            doc = copy.deepcopy(base)
            mutate(doc)
            rc, out = run(doc, work, name.replace(' ', '-'))
            assert rc == 1 and needle in out, (name, rc, out)
    print(f'rsx-frame checker PASS: the matching capture accepted, {len(cases)} broken fields each refused')
    return 0


if __name__ == '__main__':
    sys.exit(main())
