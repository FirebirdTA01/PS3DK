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


def abort_functions(src):
    def body(name):
        start = re.search(r'static (?:unsigned|void|int) ' + name + r'\([^\n]*\) \{', src)
        assert start, 'missing abort gate function: ' + name
        end, depth = src.index('{', start.start()) + 1, 1
        while depth:
            depth += (src[end] == '{') - (src[end] == '}')
            end += 1
        return src[start.start():end]
    release = body('release_gates')
    recovery = body('recovery_required')
    ending = body('end_run')
    assert recovery.index('release_gates()') < recovery.index('log_line(')
    assert ending.index('release_gates()') < ending.index('quiet()')
    assert src.index('gate_highest = gate;') < src.index('cellGcmSetWaitLabel(ctx, LABEL_GATE, gate)')
    assert 'gate_label = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_GATE)' in src
    return release, recovery, ending


def check_abort_paths(src, out):
    release, recovery, ending = abort_functions(src)
    prefix = r'''#include <stdint.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
static volatile uint32_t word, *gate_label;
static uint32_t gate_highest;
static int idle, failed, syncs;
static uint64_t session_start;
static jmp_buf terminal;
static uint64_t elapsed_us(void) { return 120000001; }
static void sync_memory(void) { ++syncs; }
static void observe(void) {
    if (gate_highest && (word != gate_highest || !syncs)) failed = 1;
}
#define log_line(...) (observe(), (void)printf(__VA_ARGS__), (void)putchar('\n'))
static void quiet(void) { observe(); idle = 1; }
static int usleep(unsigned n) { (void)n; longjmp(terminal, 1); }
'''
    # Compile the actual recovery and exit bodies, replacing only platform calls.
    # The queued gate must be released before logging or attempting final drain.
    suffix = r'''
int main(void) {
    (void)session_start; (void)elapsed_us;
    for (unsigned target = 0; target <= 21; target += 7) {
        gate_label = target ? &word : 0; gate_highest = target;
        word = target ? target-1 : 99; syncs = failed = 0;
        if (!setjmp(terminal)) recovery_required("abort-with-gate");
        if (failed || (target && word != target)) goto fail;
        word = target ? target-1 : 99; syncs = failed = 0; idle = 0;
        if (end_run(3, "deadline") != 3 || failed || !idle) goto fail;
    }
    puts("abort paths release queued gate before logging/draining PASS");
    return 0;
fail:
    puts("ASSERTION: abort_releases_queued_gate"); return 1;
}
'''
    for mutant in (False, True):
        tested = release.replace('*gate_label = gate_highest;', '(void)gate_label;') if mutant else release
        assert not mutant or tested != release
        source = out / ('abort-mutant.c' if mutant else 'abort.c')
        source.write_text(prefix + tested + recovery + ending + suffix)
        binary = source.with_suffix('')
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            str(source), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
        if mutant:
            assert run.returncode == 1 and run.stdout.strip().endswith('ASSERTION: abort_releases_queued_gate'), run
            print('KILLED abort_releases_queued_gate')
        else:
            assert run.returncode == 0, run.stdout + run.stderr
            print(run.stdout.strip())


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
        check_abort_paths(src, out)
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
