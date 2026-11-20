#!/usr/bin/env python3
"""Compare an rsx-frame sample capture with expected/draws.tsv, field by field.

    check_capture.py --reader ps3dk-prof --capture run.prof --shaders build/shaders
    check_capture.py --reader-json frame.json --hashes-json hashes.json      (no reader run: saved outputs)

expected/state.tsv is checked too (--state): each listed register-state field of each draw (decoder 2 captures);
expected/constants.tsv (--constants): each listed uniform value of each draw, and the table's `@ instance_runs`
(decoder 3 captures).
The reader output is `ps3dk-prof rsx-frame --json <capture>`; program names in the table are shader file stems,
mapped to canonical hashes with `ps3dk-prof shader-hash --json <dir>/*.vpo <dir>/*.fpo` (the files the build
compiled). The capture checked is the one named by the table's `@ name` (the first if several). Prints one line per
difference and a verdict; exit 0 only when everything matches, 1 on any difference, 2 on a usage or input error."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def die(msg):
    print(f'check_capture: {msg}', file=sys.stderr)
    sys.exit(2)


COLUMNS = ['ordinal', 'kind', 'primitive', 'count', 'first', 'index', 'label', 'vp', 'fp', 'tag', 'pass', 'timing']


def load_table(path):
    meta, rows = {}, []
    for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        if line.startswith('@ '):
            k, _, v = line[2:].partition('\t')
            meta[k.strip()] = v.strip()
            continue
        cells = line.split('\t')
        if len(cells) != len(COLUMNS):
            die(f'{path}:{n}: {len(cells)} columns, expected {len(COLUMNS)}')
        rows.append(dict(zip(COLUMNS, cells)))
    return meta, rows


def load_state(path):
    '''[(ordinal or None for every draw, table, field, expected value)] from a state table.'''
    rows = []
    for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        cells = line.split('\t')
        if len(cells) != 4:
            die(f'{path}:{n}: {len(cells)} columns, expected 4')
        try:
            value = json.loads(cells[3])
        except ValueError:
            die(f'{path}:{n}: value {cells[3]!r} is not a JSON value')
        rows.append((None if cells[0] == '*' else int(cells[0]), cells[1], cells[2], value))
    return rows


def compare_state(rows, doc, name):
    '''The differences between each draw's register state and the state table.'''
    bad = []
    caps = [c for c in doc.get('captures', []) if c.get('name') == name]
    if not caps:
        return bad
    draws = caps[0].get('draws', [])
    for ordinal, table, field, want in rows:
        targets = draws if ordinal is None else [d for d in draws if d.get('ordinal') == ordinal]
        for d in targets:
            where = f'draw {d.get("ordinal")} {table} {field}'
            state = d.get('state')
            if state is None:
                bad.append(f'draw {d.get("ordinal")}: no register state (a decoder 1 capture?)')
                continue
            section, _, slot = table.partition('.')
            t = state.get(section)
            if slot:
                t = t[int(slot)] if isinstance(t, list) and int(slot) < len(t) else None
            if not isinstance(t, dict) or field not in t:
                bad.append(f'{where}: no such field')
            elif t[field] != want:
                bad.append(f'{where}: {t[field]!r}, expected {want!r}')
    # one line per missing state, not per field
    return list(dict.fromkeys(bad))


def load_constants(path):
    '''[(ordinal, profile, uniform name, expected vec4)] from a constants table.'''
    rows = []
    for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        cells = line.split('\t')
        profile, _, name = cells[1].partition('.') if len(cells) == 3 else ('', '', '')
        if len(cells) != 3 or profile not in ('vp', 'fp') or not name:
            die(f'{path}:{n}: expected ordinal, vp.<name> or fp.<name>, value')
        try:
            value = json.loads(cells[2])
        except ValueError:
            die(f'{path}:{n}: value {cells[2]!r} is not JSON')
        rows.append((int(cells[0]), profile, name, value))
    return rows


def compare_constants(rows, meta, doc):
    '''The differences in uniform values and instance runs.'''
    bad = []
    caps = [c for c in doc.get('captures', []) if c.get('name') == meta.get('name')]
    if not caps:
        return bad
    c = caps[0]
    draws = {d.get('ordinal'): d for d in c.get('draws', [])}
    for ordinal, profile, name, want in rows:
        d = draws.get(ordinal)
        where = f'draw {ordinal} {profile}.{name}'
        if d is None:
            continue  # the draw table reports a missing draw
        consts = d.get('constants')
        if consts is None:
            bad.append(f'draw {ordinal}: no constants (a capture before decoder 3?)')
            continue
        u = [x for x in consts.get(profile, []) if x.get('name') == name]
        if not u:
            bad.append(f'{where}: no such uniform')
            continue
        got = (u[0].get('values') or [None])[0]
        if not isinstance(got, list) or len(got) != len(want) or any(
                g is None or abs(g - w) > 1e-5 for g, w in zip(got, want)):
            bad.append(f'{where}: {got!r}, expected {want!r}')
    if 'instance_runs' in meta:
        want_runs = []
        for item in filter(None, meta['instance_runs'].split()):
            first, length, names = item.split(':')
            want_runs.append((int(first), int(length), sorted(filter(None, names.split(',')))))
        got_runs = [(r.get('first'), r.get('length'), sorted(r.get('vp_uniforms', [])))
                    for r in c.get('instance_runs', [])]
        if got_runs != want_runs:
            bad.append(f'instance runs: {got_runs!r}, expected {want_runs!r}')
    return list(dict.fromkeys(bad))


def compare_both(meta, rows, doc, hashes):
    '''The BOTH capture named by `@ both`: its pipelined and isolated views each match the draw table, every draw is
    paired, and every program's bytes are stored and complete.'''
    if 'both' not in meta:
        return []
    name = meta['both']
    views = [c for c in doc.get('captures', []) if c.get('name') == name]
    if [v.get('half') for v in views] != ['first', 'second']:
        return [f'BOTH capture {name!r}: halves {[v.get("half") for v in views]!r}, expected first and second']
    bad = []
    for i, mode in enumerate(('pipelined', 'isolated')):
        m = dict(meta, name=name, timing_mode=mode)
        m.pop('instance_runs', None)
        base = views[0].get('draws', [{}])[-1].get('ordinal', -1) + 1 if i else 0
        bad += [f'{name} {mode}: {b}' for b in compare(m, rows, doc, hashes, view=i, base=base)]
    pairs = views[1].get('pairs', [])
    unpaired = [p for p in pairs if p.get('first') is None or p.get('second') is None]
    if len(pairs) != len(rows) or unpaired:
        bad.append(f'{name}: {len(pairs)} pairs, {len(unpaired)} unpaired; expected {len(rows)} all paired')
    for pb in views[0].get('program_bytes', []):
        if not pb.get('bytes') or pb.get('complete') is not True:
            bad.append(f'{name}: program {pb.get("index")} bytes {pb.get("bytes")!r} complete {pb.get("complete")!r}')
    return bad


def run_json(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, timeout=300)
    if r.returncode != 0:
        die(f'{" ".join(map(str, cmd))}: exit {r.returncode}: {r.stderr.decode(errors="replace")[-2000:]}')
    return json.loads(r.stdout)


def hashes_by_stem(doc):
    out = {}
    for row in doc:
        if 'error' in row:
            die(f'shader-hash: {row["file"]}: {row["error"]}')
        out[Path(row['file']).stem] = (row['profile'], row['hash'])
    return out


def compare(meta, rows, doc, hashes, view=0, base=0):
    """Returns the list of differences (empty: the capture matches): of the `view`-th view named meta['name'], whose
    ordinals start at `base`."""
    bad = []
    caps = [c for c in doc.get('captures', []) if c.get('name') == meta.get('name')][view:]
    if doc.get('availability') != 'available':
        bad.append(f'availability: {doc.get("availability")!r}, expected available')
    if 'decoder' in meta and str(doc.get('decoder')) != meta['decoder']:
        bad.append(f'decoder: {doc.get("decoder")!r}, expected {meta["decoder"]}')
    if not caps:
        return bad + [f'no capture named {meta.get("name")!r} (captures: {[c.get("name") for c in doc.get("captures", [])]})']
    c = caps[0]
    t = c.get('totals', {})
    for key, got in [('source', c.get('source')), ('status', c.get('status')), ('timing_mode', c.get('timing_mode')),
                     ('draws', len(c.get('draws', []))), ('programs', len(c.get('programs', []))),
                     ('mismatched', t.get('mismatched')), ('untimed', t.get('untimed')),
                     ('missing', t.get('missing')), ('text_replaced', c.get('text_replaced'))]:
        if key in meta and str(got) != meta[key]:
            bad.append(f'capture {key}: {got!r}, expected {meta[key]!r}')
    progs = {p['index']: p for p in c.get('programs', [])}
    for p in progs.values():
        for flag in ('unreadable', 'unterminated', 'not_revalidated'):
            if p.get(flag):
                bad.append(f'program {p["index"]} ({p.get("id")}): {flag}')
    passes = c.get('passes', [])
    draws = c.get('draws', [])
    for i, want in enumerate(rows):
        if i >= len(draws):
            bad.append(f'draw {want["ordinal"]}: missing (the capture has {len(draws)} draws)')
            continue
        d = draws[i]
        where = f'draw {want["ordinal"]}'
        index = d.get('index')
        pass_name = passes[d['pass']]['name'] if d.get('pass') is not None and d['pass'] < len(passes) else '-'
        got = {
            'ordinal': str(d.get('ordinal')), 'kind': d.get('kind'), 'primitive': d.get('primitive'),
            'count': str(d.get('count')), 'first': str(d.get('first')),
            'index': f'{index["type"]}/{index["location"]}' if index else '-',
            'label': d.get('label') or '-', 'tag': hex(d.get('tag', 0)), 'pass': pass_name,
            'timing': d.get('timing', {}).get('state'),
        }
        for col in COLUMNS:
            if col in ('vp', 'fp'):
                continue
            exp = want[col] if col != 'tag' else hex(int(want[col], 0))
            if col == 'ordinal':
                exp = str(int(exp) + base)
            if got[col] != exp:
                bad.append(f'{where} {col}: {got[col]!r}, expected {exp!r}')
        for col in ('vp', 'fp'):
            idx = d.get(col)
            if want[col] == '-':
                if idx is not None:
                    bad.append(f'{where} {col}: program {idx}, expected none')
                continue
            if want[col] not in hashes:
                bad.append(f'{where} {col}: no shader-hash for {want[col]!r}')
                continue
            profile, h = hashes[want[col]]
            p = progs.get(idx)
            if p is None:
                bad.append(f'{where} {col}: unknown, expected {want[col]} ({profile}:{h})')
            elif (p.get('profile'), p.get('hash')) != (profile, h):
                bad.append(f'{where} {col}: {p.get("profile")}:{p.get("hash")}, expected {want[col]} ({profile}:{h})')
        for flag, expect in (('hooked', True), ('decoded', True), ('mismatch', False), ('state_unknown', False)):
            if d.get(flag) is not expect:
                bad.append(f'{where} {flag}: {d.get(flag)!r}, expected {expect!r}')
    for d in draws[len(rows):]:
        bad.append(f'draw {d.get("ordinal")}: not in the table ({d.get("kind")} {d.get("label")})')
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--expected', type=Path, default=HERE.parent / 'expected/draws.tsv')
    ap.add_argument('--state', type=Path, default=HERE.parent / 'expected/state.tsv',
                    help='the register-state table; "-" skips it')
    ap.add_argument('--constants', type=Path, default=HERE.parent / 'expected/constants.tsv',
                    help='the uniform-value table; "-" skips it and the instance runs')
    ap.add_argument('--reader', type=Path, help='ps3dk-prof')
    ap.add_argument('--capture', type=Path, help='the sample\'s .prof')
    ap.add_argument('--shaders', type=Path, help='the build\'s shaders directory (*.vpo, *.fpo)')
    ap.add_argument('--reader-json', type=Path, help='saved `rsx-frame --json` output instead of --reader/--capture')
    ap.add_argument('--hashes-json', type=Path, help='saved `shader-hash --json` output instead of --shaders')
    a = ap.parse_args()
    meta, rows = load_table(a.expected)
    if a.reader_json:
        doc = json.loads(a.reader_json.read_text())
    elif a.reader and a.capture:
        doc = run_json([a.reader, 'rsx-frame', '--json', a.capture])
    else:
        ap.error('give --reader and --capture, or --reader-json')
    if a.hashes_json:
        hdoc = json.loads(a.hashes_json.read_text())
    elif a.reader and a.shaders:
        files = sorted(a.shaders.glob('*.vpo')) + sorted(a.shaders.glob('*.fpo'))
        if not files:
            die(f'{a.shaders}: no .vpo or .fpo files')
        hdoc = run_json([a.reader, 'shader-hash', '--json', *files])
    else:
        ap.error('give --reader and --shaders, or --hashes-json')
    bad = compare(meta, rows, doc, hashes_by_stem(hdoc))
    bad += compare_both(meta, rows, doc, hashes_by_stem(hdoc))
    if str(a.state) != '-':
        bad += compare_state(load_state(a.state), doc, meta.get('name'))
    if str(a.constants) != '-':
        bad += compare_constants(load_constants(a.constants), meta, doc)
    for b in bad:
        print('DIFF', b)
    print(f'rsx-frame capture {"MATCHES" if not bad else "DIFFERS from"} {a.expected.name}: {len(rows)} draws, '
          f'{len(bad)} differences')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
