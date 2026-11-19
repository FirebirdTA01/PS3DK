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


def matching(meta, rows):
    """The reader JSON of a capture equal to the table."""
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
            'state_unknown': False})
    return {'schema': 1, 'capability': True, 'availability': 'available', 'captures': [{
        'id': 1, 'name': meta['name'], 'source': meta['source'], 'status': meta['status'],
        'timing_mode': meta['timing_mode'], 'text_replaced': 0,
        'totals': {'draws': len(draws), 'timed': len(draws), 'untimed': 0, 'missing': 0, 'mismatched': 0},
        'programs': programs, 'passes': [{'name': n} for n in pass_names], 'draws': draws}]}


def run(doc, work, name):
    rj, hj = work / f'{name}.json', work / 'hashes.json'
    rj.write_text(json.dumps(doc))
    hj.write_text(json.dumps(HASHES))
    r = subprocess.run([sys.executable, '-I', str(TOOL), '--reader-json', str(rj), '--hashes-json', str(hj)],
                       capture_output=True, timeout=60)
    return r.returncode, r.stdout.decode()


def main():
    meta, rows = cc.load_table(TOOL.parent.parent / 'expected/draws.tsv')
    assert len(rows) == int(meta['draws']) == 16, (len(rows), meta)
    base = matching(meta, rows)

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
        'extra draw': (lambda x: x['captures'][0]['draws'].append(dict(d(x, 15), ordinal=16)), 'draw 16'),
        'missing draw': (lambda x: x['captures'][0]['draws'].pop(), 'draw 15: missing'),
        'clear with fp': (lambda x: d(x, 0).__setitem__('fp', 1), 'draw 0 fp'),
        'no capture': (lambda x: x['captures'][0].__setitem__('name', 'other'), 'no capture named'),
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
