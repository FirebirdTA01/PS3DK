#!/usr/bin/env python3
"""The public AUTO range/options macros of <ps3dk/prof_auto.h> as int expressions, in C and C++.

Every form callers use (a statement, an unbraced if/else arm, a comma-free expression, an assigned or compared
result) compiles with -Wall -Wextra -Werror, as C11 and C++17, with PS3TC_PROF_AUTO on (compile only: the runtime is
not linked) and off (linked and run: BEGIN, BEGIN_TIMED and END yield 1, OPTIONS_SET 0, and nothing is called).
--mutant void-macros puts back the earlier (void) macro forms and must fail to compile.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--mutant', choices=['void-macros'])
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
src = root/'sdk/libps3dkprof/src'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out/'include/ps3dk'
inc.mkdir(parents=True)
for name in ('prof.h', 'prof_fifo.h', 'prof_auto.h'):
    shutil.copyfile(root/'sdk/include/ps3dk'/name, inc/name)
for name, rel in [('prof_gpu.h', 'rsx/prof_gpu.h'), ('prof_spu.h', 'spu/prof_spu.h'), ('psp_desc.h', 'spu/psp_desc.h')]:
    shutil.copyfile(src/rel, inc/name)
if a.mutant == 'void-macros':
    h = inc/'prof_auto.h'
    code = h.read_text()
    old = '#define PS3DK_PROF_AUTO_RANGE_END() ps3dk_prof_auto_range_end()'
    assert code.count(old) == 1
    h.write_text(code.replace(old, '#define PS3DK_PROF_AUTO_RANGE_END() ((void)ps3dk_prof_auto_range_end())'))

USES = r'''
#include <ps3dk/prof_auto.h>
#include <stdio.h>
static int counted;
int use_ranges(int flag)
{
    int admitted = 0, committed = 0;
    PS3DK_PROF_AUTO_RANGE_BEGIN("statement");
    PS3DK_PROF_AUTO_RANGE_END();
    if (flag) PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED("arm"); else PS3DK_PROF_AUTO_RANGE_BEGIN("arm");
    if (flag) PS3DK_PROF_AUTO_RANGE_END(); else PS3DK_PROF_AUTO_RANGE_END();
    admitted += PS3DK_PROF_AUTO_RANGE_BEGIN("counted");
    committed += PS3DK_PROF_AUTO_RANGE_END();
    if (!PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED("refused?")) counted++;
    if (PS3DK_PROF_AUTO_RANGE_END() == 0) counted++;
    for (int i = 0; i < 2; i++) { admitted += PS3DK_PROF_AUTO_RANGE_BEGIN("loop"); committed += PS3DK_PROF_AUTO_RANGE_END(); }
    PS3DK_PROF_AUTO_TAG(7);
    PS3DK_PROF_AUTO_TIMED_RANGES_ONLY();
    return admitted * 16 + committed;
}
int use_options(void)
{
    ps3dk_prof_auto_options o = PS3DK_PROF_AUTO_OPTIONS_INIT;
    o.range_pairs = 100;
    PS3DK_PROF_AUTO_OPTIONS_SET(&o);
    return PS3DK_PROF_AUTO_OPTIONS_SET(&o);
}
'''
MAIN = r'''
int use_ranges(int flag);
int use_options(void);
int main(void) { return use_ranges(1) == 3 * 16 + 3 && use_options() == 0 ? 0 : 1; }
'''
(out/'uses.c').write_text(USES, encoding='utf-8')
(out/'uses.cpp').write_text('extern "C" {\n' + USES.replace('#include <ps3dk/prof_auto.h>\n#include <stdio.h>\n', '')
                            .replace('static int counted;', '}\n#include <ps3dk/prof_auto.h>\nextern "C" {\nstatic int counted;')
                            + '}\n', encoding='utf-8')
(out/'main.c').write_text(MAIN, encoding='utf-8')
cc, cxx = os.environ.get('CC', 'cc'), os.environ.get('CXX', 'c++')
warn = ['-Wall', '-Wextra', '-Werror', '-I'+str(inc.parent)]
log = []


def run(cmd):
    r = subprocess.run(list(map(str, cmd)), capture_output=True, text=True, timeout=120)
    log.append((cmd, r.returncode, r.stderr))
    return r


steps = []
for on in (1, 0):
    d = ['-DPS3TC_PROF_AUTO=1'] if on else []
    steps.append(([cc, '-std=c11', *warn, *d, '-c', out/'uses.c', '-o', out/f'uses-c-{on}.o'], f'C on={on}'))
    steps.append(([cxx, '-std=c++17', *warn, *d, '-c', out/'uses.cpp', '-o', out/f'uses-cxx-{on}.o'], f'C++ on={on}'))
failed = []
for cmd, what in steps:
    r = run(cmd)
    if r.returncode:
        failed.append((what, r.stderr))
(out/'log.txt').write_text('\n'.join(f'{c}\nrc={rc}\n{e}' for c, rc, e in log), encoding='utf-8')
if a.mutant:
    assert failed and all('on=1' in w for w, _ in failed), failed
    print('void-macros rejected: the (void) END form is not an int expression:', ', '.join(w for w, _ in failed))
    raise SystemExit(0)
assert not failed, failed
for lang, obj in (('C', 'uses-c-0.o'), ('C++', 'uses-cxx-0.o')):
    linker = cc if lang == 'C' else cxx
    r = run([linker, out/'main.c', out/obj, '-o', out/f'app-{lang}'] if lang == 'C' else
            [cc, '-c', out/'main.c', '-o', out/'main.o'])
    assert r.returncode == 0, r.stderr
    if lang == 'C++':
        r = run([cxx, out/'main.o', out/obj, '-o', out/f'app-{lang}'])
        assert r.returncode == 0, r.stderr
    r = run([out/f'app-{lang}'])
    assert r.returncode == 0, (lang, 'off-build values', r.returncode)
print('AUTO range and options macros: int expressions in C and C++, on and off, warning-free PASS')
