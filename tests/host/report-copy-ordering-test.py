#!/usr/bin/env python3
"""Ordering detector/packet controls; no GPU execution."""
import os, re, shlex, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SAMPLE=ROOT/'samples/gcm/hello-ppu-gcm-report-copy-ordering'
assert (SAMPLE/'source/ordering_logic.h').exists(), 'ordering implementation missing'
with tempfile.TemporaryDirectory(prefix='ordering-host-') as tmp:
    for mutant, reason in [(0,None),(1,'last_byte'),(2,'frozen_early'),(3,'distant_guard'),(4,'control_required'),(5,'packet'),(6,'schedule'),(7,'fresh_generation')]:
        exe=Path(tmp)/f'test-{mutant}'
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',f'-DORD_MUTANT={mutant}',str(ROOT/'tests/host/report-copy-ordering-test.c'),'-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
        assert (r.returncode==1 and r.stdout.strip()=='ASSERTION: '+reason) if reason else r.returncode==0,(mutant,r)
        print('KILLED '+reason if reason else r.stdout.strip())
src=(SAMPLE/'source/main.c').read_text()
base=(ROOT/'samples/gcm/hello-ppu-gcm-report-copy-b0/source/main.c').read_text()
def body(s,name):
    m=re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\) \{',s,re.M);assert m,name
    i=s.index('{',m.start())+1;level=1
    while level:level+=(s[i]=='{')-(s[i]=='}');i+=1
    return s[m.start():i]
for name in ('release_gates','recovery_required','next_serial','reserve_batch','backend_gate','await_label','emit_final','await_final','quiet','snapshot','end_run'):
    assert body(src,name)==body(base,name).replace('B0_', 'ORD_'),name+' drift'
assert 'gate_highest = gate;' in src and src.index('gate_highest = gate;')<src.index('cellGcmSetWaitLabel(ctx, LABEL_GATE, gate)')
assert src.index('snapshot(immediate,')<src.index('await_final(final,ref)')
assert 'reports[i] =' not in src and '*source =' not in src and 'reset_slots(' not in src
assert '../../hello-ppu-gcm-report-ring/source/state_tour_setup.h' in src
assert 'ord_packet(' in src and 'ord_evidence(' in src and 'ord_result(' in src
assert '0x1400' not in src and 'cellGcmAddressToOffset' not in src
import importlib.util
spec=importlib.util.spec_from_file_location('ring_gate',ROOT/'tests/host/report-ring-probe-test.py')
ring=importlib.util.module_from_spec(spec);spec.loader.exec_module(ring)
with tempfile.TemporaryDirectory(prefix='ordering-abort-') as tmp:
    ring.check_abort_paths(src,Path(tmp))
print('report-copy-ordering-test PASS')
