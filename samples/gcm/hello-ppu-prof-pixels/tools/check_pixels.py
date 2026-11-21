#!/usr/bin/env python3
"""Checks a hello-ppu-prof-pixels capture against its known answers.

For each of the sample's three captures (`ps3dk-prof rsx-frame --json`):
  pixels       (PIXELS)        every quad counted: forced, except app_query (the app's own counting); samples as the
                               table, in terms of the draw's clip width x height;
  observe      (no PIXELS)     only app_query counted (W x H); the other quads are 'off' with no sample count;
  pixels-both  (BOTH | PIXELS) both views as 'pixels', and each paired draw's samples equal in the two views
                               (pipelined and isolated counts agree).
Every capture: zcull region 0 bound before the capture over the depth buffer, every quad's zcull effective, and
'front' marked zcull-invalidated. With --tty, every 'PIXELS appquery' line has value == expect (the capture never
changed the app's own query result in a captured frame). Prints one DIFF line per difference; exit 0 only on a full match.

usage: check_pixels.py --reader <ps3dk-prof> --capture <file.prof> [--tty TTY.log]"""
import argparse
import json
import re
import subprocess
import sys

FULL, HALF, NONE = 'full', 'half', 'none'
# label -> expected passed samples (clears are not checked: their counter semantics are not characterized)
TABLE = {'full': FULL, 'half': HALF, 'front': FULL, 'behind': NONE, 'back_first': FULL, 'front_second': FULL,
         'kil_half': HALF, 'app_query': FULL}
ORDER = ['clear', 'full', 'half', 'clear_depth', 'front', 'behind', 'clear_depth2', 'back_first', 'front_second',
         'kil_half', 'app_query']


def expected(kind, d):
    st = d.get('state', {}).get('surfaces', {})
    w, h = st.get('clip.width'), st.get('clip.height')
    if w is None or h is None:
        return None
    return {FULL: w * h, HALF: w * h // 2, NONE: 0}[kind]


def check_view(name, c, mode, diffs):
    draws = c['draws']
    labels = [(d['label'] or '').split('/')[-1] for d in draws]
    if labels != ORDER:
        diffs.append(f'{name}: draws {labels}, expected {ORDER}')
        return {}
    out = {}
    for d, label in zip(draws, labels):
        px = d['pixels']
        if label not in TABLE:
            continue
        want = expected(TABLE[label], d)
        if mode == 'observe' and label != 'app_query':
            if px['state'] != 'off' or px['samples'] is not None:
                diffs.append(f"{name} {label}: state {px['state']} samples {px['samples']}, expected off / none")
            continue
        want_state = 'counting' if label == 'app_query' else 'forced'
        if px['state'] != want_state:
            diffs.append(f"{name} {label}: state {px['state']}, expected {want_state}")
        if want is None or px['samples'] != want:
            diffs.append(f"{name} {label}: samples {px['samples']}, expected {want}")
        if d['timing']['state'] != 'timed':
            diffs.append(f"{name} {label}: timing {d['timing']['state']}")
        z = d['zcull']
        if z['effective'] is not True or z['region'] != 0:
            diffs.append(f"{name} {label}: zcull effective {z['effective']} region {z['region']} ({z['reason']})")
        if (label == 'front') != z['invalidated']:
            diffs.append(f"{name} {label}: zcull invalidated {z['invalidated']}")
        if label == 'kil_half' and px['early_z'] != 'off (the program kills fragments)':
            diffs.append(f"{name} {label}: early-z {px['early_z']}")
        out[d['ordinal']] = px['samples']
    regions = c.get('zcull_regions', [])
    if not any(r['index'] == 0 and r['bound'] and r['before_capture'] for r in regions):
        diffs.append(f'{name}: zcull region 0 not recorded as bound before the capture ({regions})')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--reader', required=True)
    ap.add_argument('--capture', required=True)
    ap.add_argument('--tty')
    a = ap.parse_args()
    r = subprocess.run([a.reader, 'rsx-frame', '--json', a.capture], capture_output=True, text=True)
    if r.returncode not in (0, 1):
        print(f'DIFF reader exit {r.returncode}: {r.stderr.strip()}')
        return 1
    v = json.loads(r.stdout)
    diffs = []
    caps = {}
    for c in v['captures']:
        caps.setdefault(c['name'], []).append(c)
    for name, mode in (('pixels', 'pixels'), ('observe', 'observe'), ('pixels-both', 'pixels')):
        views = caps.get(name, [])
        want = 2 if name == 'pixels-both' else 1
        if len(views) != want:
            diffs.append(f'{name}: {len(views)} views, expected {want}')
            continue
        got = [check_view(f"{name}[{c['half']}]", c, mode, diffs) for c in views]
        if name == 'pixels-both' and all(got):
            second = views[1]
            for p in second['pairs']:
                if p['first'] is None or p['second'] is None:
                    diffs.append(f'{name}: unpaired draw {p}')
                    continue
                a0 = views[0]['draws'][p['first']]
                b0 = second['draws'][p['second']]
                if a0['pixels']['samples'] != b0['pixels']['samples']:
                    diffs.append(f"{name}: {a0['label']} pipelined {a0['pixels']['samples']} != isolated "
                                 f"{b0['pixels']['samples']}")
    if a.tty:
        lines = re.findall(r'PIXELS appquery frame=(\d+) value=(\d+) expect=(\d+)', open(a.tty, errors='replace').read())
        if not lines:
            diffs.append('tty: no PIXELS appquery line')
        captured = {'31', '61', '91', '92'}  # the frames each capture recorded
        if not captured <= {f for f, _, _ in lines}:
            diffs.append(f'tty: app query lines for the captured frames missing ({sorted(captured)})')
        for f, val, exp in lines:
            if f in captured and val != exp:
                diffs.append(f'tty: app query frame {f}: {val} != {exp}')
    for d in diffs:
        print('DIFF', d)
    print(f"check_pixels: {'MATCH' if not diffs else str(len(diffs)) + ' differences'}")
    return 0 if not diffs else 1


if __name__ == '__main__':
    sys.exit(main())
