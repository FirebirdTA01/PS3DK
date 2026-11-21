#!/usr/bin/env python3
"""Host test of hello-ppu-prof-pixels' checker (tools/check_pixels.py), no capture needed.

Builds the reader JSON a matching run produces (its three captures, reader field names) and a matching TTY, requires
the checker to accept them, then breaks one thing at a time and requires the checker to refuse each with a DIFF line
naming it. The reader is a stand-in script that prints the JSON. Exit 0 only when every case behaved."""
import copy
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'tools/check_pixels.py'
W, H = 1280, 720
ORDER = ['clear', 'full', 'half', 'clear_depth', 'front', 'behind', 'clear_depth2', 'back_first', 'front_second',
         'kil_half', 'app_query']
SAMPLES = {'full': W * H, 'half': W * H // 2, 'front': W * H, 'behind': 0, 'back_first': W * H, 'front_second': W * H,
           'kil_half': W * H // 2, 'app_query': W * H}


def draws(mode):
    out = []
    for i, label in enumerate(ORDER):
        clear = label.startswith('clear')
        if clear:
            state, samples = 'forced', None
        elif mode == 'observe' and label != 'app_query':
            state, samples = 'off', None
        else:
            state, samples = ('counting' if label == 'app_query' else 'forced'), SAMPLES[label]
        out.append({'ordinal': i, 'label': 'pixels/' + label, 'timing': {'state': 'timed'},
                    'state': {'surfaces': {'clip.width': W, 'clip.height': H}},
                    'pixels': {'state': state, 'samples': samples,
                               'early_z': 'off (the program kills fragments)' if label == 'kil_half' else 'eligible'},
                    'zcull': {'effective': True, 'region': 0, 'invalidated': label == 'front', 'reason': 'covered'}})
    return out


def capture(name, mode, half='only'):
    c = {'name': name, 'half': half, 'draws': draws(mode), 'pairs': [],
         'zcull_regions': [{'index': 0, 'bound': True, 'before_capture': True}]}
    return c


def matching():
    first, second = capture('pixels-both', 'pixels', 'first'), capture('pixels-both', 'pixels', 'second')
    # a draw of the firmware's flip routine before each view's draws: left out of the comparison
    for c in (first, second):
        c['draws'].insert(0, {'ordinal': -1, 'label': None, 'firmware': True})
    second['pairs'] = [{'first': 0, 'second': 0}]
    second['pairs'] += [{'first': i + 1, 'second': i + 1} for i in range(len(ORDER))]
    return {'captures': [capture('pixels', 'pixels'), capture('observe', 'observe'), first, second]}


TTY = ''.join(f'PIXELS appquery frame={f} value={W * H} expect={W * H}\n' for f in (31, 61, 91, 92))


def run(tmp, doc, tty=TTY):
    j = tmp / 'doc.json'
    j.write_text(json.dumps(doc))
    t = tmp / 'TTY.log'
    t.write_text(tty)
    r = subprocess.run([sys.executable, '-B', str(TOOL), '--reader', str(tmp / 'reader'), '--capture', str(j),
                        '--tty', str(t)], capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        reader = tmp / 'reader'
        reader.write_text(f'#!{sys.executable}\nimport sys\nsys.stdout.write(open(sys.argv[3]).read())\n')
        reader.chmod(reader.stat().st_mode | stat.S_IXUSR)
        bad = 0
        rc, out = run(tmp, matching())
        if rc != 0 or 'MATCH' not in out:
            print('FAIL matching run refused:\n' + out)
            bad += 1
        else:
            print('ok   matching run accepted')

        def case(name, mutate, want, tty=TTY):
            nonlocal bad
            doc = copy.deepcopy(matching())
            mutate(doc)
            rc, out = run(tmp, doc, tty)
            if rc == 1 and want in out:
                print(f'ok   {name}: refused ({want})')
            else:
                print(f'FAIL {name}: rc={rc}\n{out}')
                bad += 1

        def draw(doc, cap, label):
            return doc['captures'][cap]['draws'][ORDER.index(label)]
        case('a wrong count', lambda doc: draw(doc, 0, 'half')['pixels'].update(samples=W * H), 'pixels[only] half: samples')
        case('behind counted', lambda doc: draw(doc, 0, 'behind')['pixels'].update(samples=5), 'behind: samples 5')
        case('not forced', lambda doc: draw(doc, 0, 'full')['pixels'].update(state='off', samples=None), 'expected forced')
        case('observe counts a quad', lambda doc: draw(doc, 1, 'full')['pixels'].update(state='forced', samples=W * H),
             'observe[only] full')
        case('app query not counting', lambda doc: draw(doc, 0, 'app_query')['pixels'].update(state='forced'),
             'expected counting')
        case('zcull not effective', lambda doc: draw(doc, 0, 'full')['zcull'].update(effective=False), 'zcull effective')
        case('invalidate missing', lambda doc: draw(doc, 0, 'front')['zcull'].update(invalidated=False),
             'zcull invalidated')
        case('kil early-z', lambda doc: draw(doc, 0, 'kil_half')['pixels'].update(early_z='eligible'), 'early-z')
        case('region missing', lambda doc: doc['captures'][0].update(zcull_regions=[]), 'zcull region 0')
        case('pipelined != isolated', lambda doc: doc['captures'][3]['draws'][1 + ORDER.index('full')]['pixels'].update(samples=W * H - 1),
             'pipelined')
        case('a capture missing', lambda doc: doc['captures'].pop(1), 'observe: 0 views')
        case('draw order', lambda doc: doc['captures'][0]['draws'].reverse(), 'draws [')
        case('app query changed', lambda doc: None, 'tty: app query frame 31',
             tty=TTY.replace(f'frame=31 value={W * H}', 'frame=31 value=5'))
        case('app query line missing', lambda doc: None, 'captured frames missing', tty=TTY.replace('frame=61', 'frame=62'))
        # a late result outside the captured frames (an emulator's first frames) is not a difference
        rc, out = run(tmp, matching(), 'PIXELS appquery frame=0 value=0 expect=1\n' + TTY)
        if rc != 0:
            print('FAIL an early late result refused:\n' + out)
            bad += 1
        else:
            print('ok   a late result outside the captured frames accepted')
        print('check_pixels host test', 'PASS' if not bad else f'FAIL ({bad})')
        return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
