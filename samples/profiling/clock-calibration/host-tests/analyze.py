#!/usr/bin/env python3
"""Describe a clock-calibration probe file: RAW brackets of a foreign counter (the SPU decrementer, the RSX
timestamp) between two PPU timebase reads.

usage: analyze.py <clockcal-*.csv>

Exit status (the verdict line says the same in words):
  0  RECORD COMPLETE, run ok, cleanup ok, no domain stopped or unavailable
  1  INVALID: the file or its data break the format rules (nothing in it is relied on)
  2  RECORD INCOMPLETE: no end row (the record was cut short)
  3  RECORD COMPLETE but the run failed, cleanup failed / left the SPU group running, or a domain stopped
Domain status is reported separately from the record: USABLE (exploratory only), TOO SHORT, NO FITTABLE SEGMENT,
CONSTANT, UNAVAILABLE, NO SAMPLES, plus "; STOPPED" (lifecycle) and, for the SPU, "; NO CHANGE OBSERVED across the
spin" (the paired reads around the fixed spin were equal; nothing is inferred about the counter between requests).
Validation also requires: each sample bracket to start after the previous sample's bracket ended (one request in
flight), no sample after the cleanup row or after its domain stopped, and notes consistent with codes.

What it does NOT do: certify a mapping. Every fit printed is an EXPLORATORY midpoint regression inside one counter
segment; the mapping stays UNMEASURED. Units are raw counter ticks per timebase tick.

Segments: the SPU decrementer must strictly DECREASE and the RSX timestamp strictly INCREASE from sample to sample;
any other step ends the segment (a decrementer increase may be a wrap OR a reset: ambiguous, never joined).
Segments and phases are never joined. Fits need >= MIN_FIT samples and >1 distinct value; origins are integers."""
import csv
import statistics
import sys

MIN_FIT = 20
COLUMNS = ['kind', 'phase', 'index', 'tb0', 'tb1', 'value', 'note']
KINDS = {'meta', 'spu', 'rsx', 'spu-stop', 'rsx-stop', 'rsx-unavailable', 'cleanup', 'end'}
U32, U64 = (1 << 32) - 1, (1 << 64) - 1


class Invalid(Exception):
    pass


def segments(rows, down):
    segs, cur, reasons = [], [], []
    for r in rows:
        if cur:
            step = r['value'] - cur[-1]['value']
            if not (step < 0 if down else step > 0):
                reasons.append('unchanged' if step == 0 else
                               ('increase: wrap or reset (ambiguous, not joined)' if down else 'decrease'))
                segs.append(cur)
                cur = []
        cur.append(r)
    if cur:
        segs.append(cur)
    return segs, reasons


def fit_segment(label, seg, down):
    n = len(seg)
    if n < MIN_FIT:
        print(f'  {label}: n={n} TOO SHORT to fit (minimum {MIN_FIT})')
        return False
    if len({r['value'] for r in seg}) < 2:
        print(f'  {label}: n={n} CONSTANT: UNMEASURED')
        return False
    v0, t0 = seg[0]['value'], seg[0]['tb0']
    xs = [(v0 - r['value']) if down else (r['value'] - v0) for r in seg]
    ys = [((r['tb0'] - t0) + (r['tb1'] - t0)) / 2 for r in seg]
    mx, my = sum(xs) / n, sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    a = my - b * mx
    res = [a + b * x - y for x, y in zip(xs, ys)]
    widths = [r['tb1'] - r['tb0'] for r in seg]
    outside = sum(1 for x, r in zip(xs, seg) if not (r['tb0'] - t0 <= a + b * x <= r['tb1'] - t0))
    print(f'  {label}: n={n} span={seg[-1]["tb1"] - seg[0]["tb0"]} tb ticks; EXPLORATORY midpoint fit '
          f'(mapping UNMEASURED): slope {b:.9f} timebase ticks per counter tick; residual rms '
          f'{statistics.pstdev(res):.1f} max |{max(abs(x) for x in res):.1f}|; bracket width min {min(widths)} '
          f'median {statistics.median(widths)} max {max(widths)}; fitted outside own bracket {outside}/{n} (cannot rule '
          f'out asymmetric bias)')
    return True


def describe(name, rows, down, stopped, unavailable):
    """Prints the domain and returns its status word."""
    if unavailable:
        print(f'{name}: UNAVAILABLE ({unavailable})')
        return 'UNAVAILABLE'
    if not rows:
        print(f'{name}: NO SAMPLES' + (f' (stopped: {stopped})' if stopped else ''))
        return 'STOPPED' if stopped else 'NO SAMPLES'
    if len({r['value'] for r in rows}) == 1:
        print(f'{name}: {len(rows)} samples, CONSTANT value {rows[0]["value"]}: UNMEASURED')
        status = 'CONSTANT'
    else:
        segs, reasons = segments(rows, down)
        print(f'{name}: {len(rows)} samples in {len(segs)} segment(s)' +
              (f'; breaks: {"; ".join(sorted(set(reasons)))} x{len(reasons)}' if reasons else ''))
        fitted = [fit_segment(f'segment {i}', s, down) for i, s in enumerate(segs)]
        if any(fitted):
            status = 'USABLE (exploratory)'
        elif len(segs) == 1:
            status = 'TOO SHORT'
        else:
            status = 'NO FITTABLE SEGMENT'
    if stopped:
        print(f'{name}: STOPPED after these samples ({stopped})')
        return status + '; STOPPED'
    return status


def load(path):
    try:
        f = open(path, newline='')
    except OSError as e:
        raise Invalid(str(e))
    with f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            raise Invalid('empty file')
        except csv.Error as e:
            raise Invalid(str(e))
        if header != COLUMNS:
            raise Invalid(f'columns {header}, want {COLUMNS}')
        rows = []
        try:
            for n, fields in enumerate(reader, start=2):
                if len(fields) != len(COLUMNS):
                    raise Invalid(f'row {n} has {len(fields)} fields, want {len(COLUMNS)}')
                r = dict(zip(COLUMNS, fields))
                if r['kind'] not in KINDS:
                    raise Invalid(f'row {n} unknown kind {r["kind"]!r}')
                try:
                    for k in ('phase', 'index', 'tb0', 'tb1', 'value'):
                        r[k] = int(r[k])
                except ValueError:
                    raise Invalid(f'row {n} has a non-integer field')
                if any(not 0 <= r[k] <= U64 for k in ('phase', 'index', 'tb0', 'tb1', 'value')):
                    raise Invalid(f'row {n} has a value out of range')
                r['line'] = n
                rows.append(r)
        except csv.Error as e:
            raise Invalid(str(e))
    return rows


def validate(rows):
    if not rows or rows[0]['kind'] != 'meta' or sum(r['kind'] == 'meta' for r in rows) != 1:
        raise Invalid('the first row must be the only meta row')
    if rows[0]['tb0'] == 0:
        raise Invalid('meta timebase frequency is 0')
    ends = [i for i, r in enumerate(rows) if r['kind'] == 'end']
    if len(ends) > 1:
        raise Invalid(f'{len(ends)} end rows')
    if ends and ends[0] != len(rows) - 1:
        raise Invalid(f'data after the end row (line {rows[ends[0]]["line"]})')
    if sum(r['kind'] == 'cleanup' for r in rows) > 1:
        raise Invalid('more than one cleanup row')
    for kind, phases in (('spu', {0, 1}), ('rsx', {0})):
        last = {}
        for r in rows:
            if r['kind'] != kind:
                continue
            if r['phase'] not in phases:
                raise Invalid(f'line {r["line"]}: {kind} phase {r["phase"]}')
            if kind == 'spu' and r['value'] > U32:
                raise Invalid(f'line {r["line"]}: decrementer above 32 bits')
            if r['phase'] in last and r['index'] <= last[r['phase']]:
                raise Invalid(f'line {r["line"]}: {kind} index not increasing')
            last[r['phase']] = r['index']
            if r['tb1'] <= r['tb0']:
                raise Invalid(f'line {r["line"]}: bracket tb1 <= tb0')
            if kind == 'spu' and r['note']:
                if not r['note'].startswith('dec2='):
                    raise Invalid(f'line {r["line"]}: unexpected note {r["note"]!r}')
                try:
                    d2 = int(r['note'][5:])
                except ValueError:
                    raise Invalid(f'line {r["line"]}: malformed dec2')
                if not 0 <= d2 <= U32:
                    raise Invalid(f'line {r["line"]}: dec2 out of range')
                r['dec2'] = d2
    cleanup_at = next((i for i, r in enumerate(rows) if r['kind'] == 'cleanup'), None)
    stopped_at = {}
    prev_tb1 = None
    for i, r in enumerate(rows):
        if r['kind'] in ('spu-stop', 'rsx-stop'):
            stopped_at.setdefault(r['kind'][:3], i)
        if r['kind'] in ('spu', 'rsx'):
            if cleanup_at is not None and i > cleanup_at:
                raise Invalid(f'line {r["line"]}: {r["kind"]} sample after the cleanup row')
            if r['kind'] in stopped_at:
                raise Invalid(f'line {r["line"]}: {r["kind"]} sample after that domain stopped')
            # one request in flight at a time: each bracket starts after the previous completed bracket ended
            if prev_tb1 is not None and r['tb0'] < prev_tb1:
                raise Invalid(f'line {r["line"]}: bracket starts before the previous sample\'s bracket ended')
            prev_tb1 = r['tb1']
    for r in rows:
        if r['kind'] == 'cleanup':
            ok_note = r['note'] == 'ok'
            if ok_note != (r['value'] == 0) or (ok_note and r['tb0'] != 0):
                raise Invalid(f'line {r["line"]}: cleanup note {r["note"]!r} contradicts code {r["value"]} / '
                              f'left_running {r["tb0"]}')
        if r['kind'] == 'end' and (r['note'] == 'ok') != (r['value'] == 0):
            raise Invalid(f'line {r["line"]}: end note {r["note"]!r} contradicts code {r["value"]}')
    if ends:
        e = rows[ends[0]]
        n_spu = sum(r['kind'] == 'spu' for r in rows)
        n_rsx = sum(r['kind'] == 'rsx' for r in rows)
        if (e['tb0'], e['tb1']) != (n_spu, n_rsx):
            raise Invalid(f'end row counts spu={e["tb0"]} rsx={e["tb1"]} but the file has spu={n_spu} rsx={n_rsx}')


def main(path):
    try:
        rows = load(path)
        validate(rows)
    except Invalid as e:
        print(f'VERDICT INVALID: {e}')
        return 1
    print(f'timebase_hz={rows[0]["tb0"]}')
    stops = {}
    unavailable = ''
    for r in rows:
        if r['kind'] in ('spu-stop', 'rsx-stop'):
            stops.setdefault(r['kind'][:3], f'phase {r["phase"]} index {r["index"]} reason {r["note"]}')
        if r['kind'] == 'rsx-unavailable':
            unavailable = unavailable or r['note']
    spu = [r for r in rows if r['kind'] == 'spu']
    checks = [r for r in spu if 'dec2' in r]
    if checks:
        moved = sum(1 for r in checks if r['dec2'] != r['value'])
        print(f'spu paired reads around the fixed spin: the value changed in {moved} of {len(checks)} samples' +
              (' (no change observed in those reads)' if moved == 0 else ''))
    status = {}
    spu_status = []
    for ph in (0, 1):
        ph_rows = [r for r in spu if r['phase'] == ph]
        if ph_rows or ph == 0:
            spu_status.append(describe(f'spu-decrementer phase {ph}', ph_rows, True,
                                       stops.get('spu') if ph_rows or ph == 0 else '', ''))
    status['spu'] = ', '.join(spu_status) + ('; NO CHANGE OBSERVED across the spin' if checks and moved == 0 else '')
    status['rsx'] = describe('rsx-timestamp', [r for r in rows if r['kind'] == 'rsx'], False, stops.get('rsx'),
                             unavailable)
    cleanup = next((r for r in rows if r['kind'] == 'cleanup'), None)
    if cleanup:
        print(f'cleanup: {cleanup["note"]} code {cleanup["value"]} left_running={cleanup["tb0"]}')
    print('domains: ' + '; '.join(f'{k} {v}' for k, v in status.items()))
    if rows[-1]['kind'] != 'end':
        print('VERDICT RECORD INCOMPLETE: no end row (the record was cut short)')
        return 2
    e = rows[-1]
    problems = []
    if e['note'] != 'ok' or e['value'] != 0:
        problems.append(f'run failed ({e["note"]}, code {e["value"]})')
    if not cleanup:
        problems.append('no cleanup row')
    elif cleanup['note'] != 'ok' or cleanup['value'] != 0 or cleanup['tb0']:
        problems.append(f'cleanup failed ({cleanup["note"]}, left_running={cleanup["tb0"]})')
    if stops or unavailable:
        problems.append('a domain stopped or was unavailable')
    if problems:
        print('VERDICT RECORD COMPLETE, RUN NOT CLEAN: ' + '; '.join(problems))
        return 3
    print(f'VERDICT RECORD COMPLETE: spu samples {e["tb0"]}, rsx samples {e["tb1"]} (no calibration claim)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
