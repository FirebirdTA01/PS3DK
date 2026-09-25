#!/usr/bin/env python3
"""Check SPURS driver selection with -###; never compiles or links inputs."""
import argparse
import itertools
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default=os.environ.get('SPU_CC'))
p.add_argument('--specs', type=Path)
p.add_argument('--output', type=Path, default=Path('build/spurs-driver-traces'))
a = p.parse_args()
if not a.cc and os.environ.get('PS3DEV'):
    a.cc = str(Path(os.environ['PS3DEV'])/'spu/bin/spu-elf-gcc')
if not a.cc:
    print('SPURS driver traces: SKIP (set SPU_CC or --cc)')
    raise SystemExit(0)
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
dummy = out / 'trace-only.o'
dummy.write_bytes(b'')
custom = out / 'custom.ld'
custom.write_text('/* Dry-run placeholder only. */\n')
base = [a.cc] + (['-specs=' + str(a.specs.resolve())] if a.specs else [])
modes = {
    'task': ('-mspurs-task', ['spurs_task.o'], ['spurs_task_runtime'], 'spurs_task.ld'),
    'job': ('-mspurs-job', ['job_start.o'], ['spurs_job_runtime'], 'spurs_job.ld'),
    'initialized': ('-mspurs-job-initialize', ['job_start_w_crt.o', 'job_crt.o'], ['spurs_jq_runtime'], 'spurs_job.ld'),
}
rows = []


def trace(name, flags):
    cmd = base + ['-###', *flags, str(dummy), '-o', str(out/'never-produced.elf')]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    (out/(name+'.log')).write_text(text)
    links = [s for s in text.splitlines() if 'collect2' in s or re.search(r'[/\\]spu-elf-ld(?:\s|\")', s)]
    tokens = re.findall(r'"([^\"]*)"|(\S+)', links[-1]) if links else []
    tokens = [x or y for x, y in tokens]
    row = dict(name=name, command=cmd, rc=proc.returncode, link=tokens)
    rows.append(row)
    return row


for mode, (flag, starts, libs, script) in modes.items():
    for variant, extra in [('default', []), ('nostartfiles', ['-nostartfiles']),
                           ('nodefaultlibs', ['-nodefaultlibs']), ('nostdlib', ['-nostdlib']),
                           ('T', ['-T', str(custom)]), ('Wl-T', ['-Wl,-T,'+str(custom)])]:
        r = trace(mode+'-'+variant, [flag, *extra])
        tokens = r['link']
        objects = [Path(t).name for t in tokens if t.endswith('.o') and t != str(dummy)]
        libraries = [t[2:] for t in tokens if t.startswith('-l')]
        want_starts = [] if variant in ('nostartfiles', 'nostdlib') else starts
        want_libs = [] if variant in ('nodefaultlibs', 'nostdlib') else libs
        r['ok'] = (r['rc'] == 0 and objects == want_starts and
                   all(lib in libraries for lib in want_libs) and
                   (bool(libraries) if want_libs else not libraries) and
                   any(t.endswith(script) for t in tokens) and
                   '--spurs-'+('job-initialize' if mode=='initialized' else mode) in tokens and
                   (variant not in ('T', 'Wl-T') or str(custom) in tokens))
    for kind, extra in [('shared', ['-shared']), ('relocatable', ['-r'])]:
        r = trace(mode+'-'+kind, [flag, *extra])
        r['ok'] = r['rc'] != 0 and not r['link']
    r = trace(mode+'-compile-only', [flag, '-c'])
    r['ok'] = r['rc'] == 0 and not r['link']

for left, right in itertools.permutations(modes, 2):
    r = trace('conflict-'+left+'-'+right, [modes[left][0], modes[right][0]])
    r['ok'] = r['rc'] != 0 and not r['link']
r = trace('ordinary', [])
objects = [Path(t).name for t in r['link'] if t.endswith('.o')]
libraries = [t for t in r['link'] if t.startswith('-l')]
r['ok'] = (r['rc']==0
    and objects == ['crt1.o', 'crti.o', 'crtbegin.o', dummy.name, 'crtend.o', 'crtn.o']
    and libraries == ['-lgcc', '-lc', '-lgloss', '-lsputhread',
                      '-lgcc_cachemgr', '-lgcc_cache64k', '-lgcc']
    and not any(
    'spurs' in Path(t).name for t in r['link']
    if t != str(dummy) and t != str(out/'never-produced.elf')))
for r in rows:
    print(r['name'], 'PASS' if r['ok'] else 'FAIL')
(out/'results.json').write_text(json.dumps(rows, indent=2)+'\n')
assert not (out/'never-produced.elf').exists(), 'Dry run unexpectedly produced an ELF'
raise SystemExit(0 if all(r['ok'] for r in rows) else 1)
