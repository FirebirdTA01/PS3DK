#!/usr/bin/env python3
"""Host gate: exact B0 command bytes and sensitive controls; no GPU execution."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SAMPLE = ROOT / 'samples/gcm/hello-ppu-gcm-report-copy-b0'
assert (SAMPLE / 'source/main.c').exists(), 'B0 sample is not implemented'
src = (SAMPLE / 'source/main.c').read_text()
assert '../../hello-ppu-gcm-report-ring/source/state_tour_setup.h' in src
assert 'reset_slots(' not in src, 'B0 must not CPU-write report sentinels'
assert 'reports[i] =' not in src and '*source =' not in src
assert 'cellGcmSetFlip(' not in src and 'cellGcmFinish(' not in src
assert 'free(' not in src and 'cellGcmBindZcull(' not in src
assert 'b0_packet(' in src and 'b0_decide(' in src
assert 'candidate < 2' in src and 'B0_MATCH' in src
assert 'B0_CONTROL omitted_copy' in src
assert src.index('B0_CONTROL omitted_copy') < src.index('candidate < 2')
assert 'if (!await_label(LABEL_FINAL' in src
recovery = src[src.index('static void recovery_required(const char *where) {'):src.index('static uint32_t next_serial')]
assert 'release_gates()' in recovery and 'for (;;) usleep' in recovery
assert not any(t in recovery for t in ('cellGcm', 'free(', 'exit(', 'exitspawn(', 'return;'))
assert 'LABEL_REPORT; label <= LABEL_FINAL; ++label)' in src
assert '= 0;\n    sync_memory();\n    for (unsigned label' in src
original = (ROOT/'samples/gcm/hello-ppu-gcm-report-ring/source/main.c').read_text()
def body(text,name):
    m=re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\) \{',text,re.M)
    assert m,name
    end=text.index('{',m.start())+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[m.start():end]
for name in ('release_gates','recovery_required','next_serial','reserve_batch','backend_gate',
             'await_label','emit_final','await_final','quiet','snapshot','end_run'):
    assert body(src,name)==body(original,name).replace('RRP_RECOVERY_REQUIRED','B0_RECOVERY_REQUIRED').replace('RRP_END','B0_END'),name+' safety helper drift'
cmake = (SAMPLE / 'CMakeLists.txt').read_text()
assert all('../hello-ppu-cellgcm-state-tour/shaders/'+s in cmake for s in ('vpshader.vcg','fpshader.fcg'))
with tempfile.TemporaryDirectory(prefix='b0-host-') as tmp:
    tmp = Path(tmp)
    for mutant, assertion in [(0,None),(1,'last_byte_mismatch'),(2,'omitted_copy_caught'),
                               (3,'trailing_guard'),(4,'timeout_stops'),(5,'exact_packet')]:
        exe=tmp/f'b0-{mutant}'
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
            f'-DB0_MUTANT={mutant}',str(ROOT/'tests/host/report-copy-b0-test.c'),'-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
        if assertion:
            assert r.returncode==1 and r.stdout.strip()=='ASSERTION: '+assertion,(mutant,r)
            print('KILLED',assertion)
        else:
            assert r.returncode==0,r
            print(r.stdout.strip())
print('report-copy-b0-test PASS; no GPU execution')
