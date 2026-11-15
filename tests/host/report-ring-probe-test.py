#!/usr/bin/env python3
"""Host checks for probe verdicts, deadlines, plans and unchanged GPU setup."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SAMPLE = ROOT / 'samples/gcm/hello-ppu-gcm-report-ring'


def function(text, name):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\)\n\{', text, re.M)
    assert match, name
    start = text.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def main():
    baseline = (ROOT / 'samples/gcm/hello-ppu-cellgcm-state-tour/source/main.c').read_text(encoding='utf-8')
    copied = (SAMPLE / 'source/state_tour_setup.h').read_text(encoding='utf-8')
    hashes = {}
    for name in ('TILE_W', 'TILE_H', 'TILE_PITCH', 'CB_SIZE', 'HOST_HEAP_SIZE'):
        pattern = r'^#define\s+' + name + r'\s+([^\n/]+)'
        old = re.search(pattern, baseline, re.M).group(1)
        new = re.search(pattern, copied, re.M).group(1)
        assert re.sub(r'\s', '', old) == re.sub(r'\s', '', new), 'setup constant drift: ' + name
    for name in ('local_align', 'init_rsx', 'init_shaders', 'init_geometry', 'bind_surface', 'env_reset'):
        original = function(baseline, name)
        assert original == function(copied, name), 'setup drift: ' + name
        hashes[name] = hashlib.sha256(original.encode()).hexdigest()
    cmake = (SAMPLE / 'CMakeLists.txt').read_text()
    for shader in ('vpshader.vcg', 'fpshader.fcg'):
        assert '../hello-ppu-cellgcm-state-tour/shaders/' + shader in cmake
    src = (SAMPLE / 'source/main.c').read_text()
    assert 'cellGcmFinish(' not in src, 'unbounded finish'
    assert 'cellGcmSetFlip(' not in src and 'cellGcmBindZcull(' not in src
    assert 'free(' not in src, 'mapped buffer freed'
    assert 'rrp_plan(claim, negative)' in src, 'tested command plan not used'
    assert src.index('snapshot(immediate, arena+DEST0, bytes)') < src.index('await_final(final, ref)'), 'late B2 snapshot'
    recovery = src[src.index('static void recovery_required(const char *where) {'):src.index('static uint32_t next_serial')]
    assert all(call not in recovery for call in ('cellGcm', 'free(', 'return;', 'exit(', 'exitspawn(')), 'recovery emits/exits'
    # Startup: each fence label is reset and read back, not merely incremented
    # from a stale cross-process value. The range covers every used fence label.
    assert 'LABEL_REPORT; label <= LABEL_FINAL; ++label)' in src
    assert '= 0;\n    sync_memory();\n    for (unsigned label' in src
    with tempfile.TemporaryDirectory(prefix='report-ring-probe-') as scratch:
        out = Path(scratch)
        for mutant, assertion in ((0, None), (1, 'deadline_before_completed_label'),
                                  (2, 'no_control_no_support'), (3, 'translated_offset_mismatch')):
            binary = out / f'probe-{mutant}'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', f'-DRRP_MUTANT={mutant}',
                str(ROOT / 'tests/host/report-ring-probe-test.c'), '-o', str(binary)]
            subprocess.run(command, check=True)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            if assertion:
                assert run.returncode == 1 and run.stdout.strip() == 'ASSERTION: ' + assertion, (mutant, run)
                print('KILLED', assertion)
            else:
                assert run.returncode == 0, run.stdout + run.stderr
                print(run.stdout.strip())
    print('setup function hashes:', json.dumps(hashes, sort_keys=True))
    print('report-ring-probe-test PASS; no GPU execution')


if __name__ == '__main__':
    main()
