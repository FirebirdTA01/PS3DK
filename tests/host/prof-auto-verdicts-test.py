#!/usr/bin/env python3
"""Host gate for hello-ppu-prof-auto-verdicts: bounded waits, stop and exit policy with sensitive controls, plus
source checks that every wait goes through the bounded helpers; no GPU execution."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SAMPLE = ROOT / 'samples/gcm/hello-ppu-prof-auto-verdicts/source'
src = (SAMPLE / 'main.c').read_text()


def body(text, name):
    m = re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\)\n?\{', text, re.M)
    assert m, name
    end = text.index('{', m.start()) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[m.start():end]


def body_after(text, marker):
    start = text.index('{', text.index(marker)) + 1
    return text[start:text.index('}', start)]


main = src[src.index('int main(void)'):]
# no unbounded polling: every flip-status and label read is a ready callback behind av_await
assert not re.search(r'while\s*\(\s*!?\s*cellGcm(GetFlipStatus|IsLabelPassed)', src), 'unbounded poll loop'
assert src.count('cellGcmGetFlipStatus(') == 1 and 'static int flip_done' in src
assert src.count('cellGcmIsLabelPassed(') == 1 and 'static int label_done' in src
# every flip submission and flip wait reports failure
assert src.count('cellGcmSetFlip(') == 1 and 'fail("submit-flip")' in body(src, 'submit_flip')
assert src.count('cellGcmWaitFlipComplete(') == 1 and 'fail("wait-flip-complete")' in body(src, 'wait_flip_complete')
for name in ('frame_a', 'frame_b', 'frame_c', 'frame_d', 'frame_e'):
    b = body(src, name)
    assert 'frame_begin()' in b and 'submit_flip()' in b, name
    assert re.search(r'return [^;]*submit_flip\(\)|if \(!submit_flip\(\)\) return 0', b), name + ' ignores submit'
assert 'session-deadline' in body(src, 'frame_begin') and 'av_stopping' in body(src, 'frame_begin')
# terminal recovery: no GPU command, seal, free, return or exitspawn, and it precedes the seal
rec = body(src, 'recovery_required')
assert 'for (;;)' in rec and '__attribute__((noreturn))' in src
assert not re.search(r'cellGcm|PS3DK_PROF|\bfree\(|\bexit|\n\s*return\b', rec), 'recovery issues work or leaves'
assert main.index('recovery_required(') < main.index('PS3DK_PROF_AUTO_SEAL();')
assert main.count('PS3DK_PROF_AUTO_SEAL();') == 1 and 'av_exit(flip_ok, fence_ok) != AV_EXIT_SEAL' in main
# no false clean success: a recorded failure returns 1 before the clean or quit line
assert main.index('if (g_run.failed)') < main.index('EXIT clean') and 'return 1;' in body_after(main, 'if (g_run.failed)')
tail = main[main.index('av_exit(flip_ok'):]
assert not re.search(r'\n\s*return\b', tail[:tail.index('PS3DK_PROF_AUTO_SEAL();')]), 'return before seal'

with tempfile.TemporaryDirectory(prefix='prof-auto-verdicts-host-') as tmp:
    tmp = Path(tmp)
    for mutant, assertion in [(0, None), (1, 'wait_bound'), (2, 'failure_stops'), (3, 'fence_timeout_recovers')]:
        exe = tmp / f'av-{mutant}'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                       f'-DAV_MUTANT={mutant}', str(ROOT / 'tests/host/prof-auto-verdicts-test.c'), '-o', str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        if assertion:
            assert r.returncode == 1 and r.stdout.strip() == 'ASSERTION: ' + assertion, (mutant, r)
            print('KILLED', assertion)
        else:
            assert r.returncode == 0, r
            print(r.stdout.strip())
print('prof-auto-verdicts-test PASS; no GPU execution')
