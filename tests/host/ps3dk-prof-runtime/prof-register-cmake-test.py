#!/usr/bin/env python3
"""cmake/ps3-prof-register.cmake (the PPU toolchain files' profiler options), configure only, with the host compiler
and a synthetic AUTO install: PS3_PROF_LIVE_AUTOSTART together with PS3_PROF_AUTO configures on any CMake, CMake 3.28
included, because CMAKE_PROJECT_INCLUDE names one chain file; both modules run, live first (the AUTO selection maps
the live archive the live module adds); a CMAKE_PROJECT_INCLUDE the project already had runs once, first, even when
the toolchain file is read twice; each option alone selects its own module only.

Control: the old registration (CMAKE_PROJECT_INCLUDE as a two-file list) must fail to configure on CMake older than
3.29 and configure on 3.29 and newer, so a run on 3.28 shows the limit is really exercised.

usage: prof-register-cmake-test.py <new output dir> [--cmake <cmake binary>]"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
p.add_argument('--cmake', default=shutil.which('cmake') or 'cmake')
a = p.parse_args()
ROOT = Path(__file__).resolve().parents[3]
OUT = a.out.resolve()
OUT.mkdir(parents=True, exist_ok=False)
SPEC = importlib.util.spec_from_file_location('guard', ROOT/'cmake/ps3-prof-auto-check.py')
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)
REGISTER = (ROOT/'cmake/ps3-prof-register.cmake').as_posix()

ver = subprocess.run([a.cmake, '--version'], capture_output=True, text=True, check=True).stdout
m = re.search(r'cmake version (\d+)\.(\d+)', ver)
VERSION = (int(m.group(1)), int(m.group(2)))
print(f'cmake {VERSION[0]}.{VERSION[1]} ({a.cmake})')


def sdk(base):
    """A synthetic AUTO install (the selector's preflight checks names and hashes, not contents)."""
    root = base/'sdk'
    for abi in ('ilp32', 'lp64'):  # the host compiler's data model picks one
        lib, inc = GUARD.locations(root, abi)
        lib.mkdir(parents=True, exist_ok=True)
        inc.mkdir(parents=True, exist_ok=True)
        for item in GUARD.ARCHIVES:
            (lib/item).write_bytes(item.encode())
        for item in GUARD.HEADERS:
            dest = inc/item
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(item.encode())
        manifest = dict(schema=1, kind='profiler-auto-install', abi=abi, source_commit='1'*40, automatic_attach=True,
                        archives={n: GUARD.sha(lib/n) for n in GUARD.ARCHIVES},
                        headers={n: GUARD.sha(inc/n) for n in GUARD.HEADERS})
        (lib/GUARD.MANIFEST).write_text(json.dumps(manifest), encoding='utf-8')
    return root


def configure(name, options, prologue, user_include=True):
    base = OUT/name
    src = base/'src'
    src.mkdir(parents=True)
    root = sdk(base)
    (src/'main.c').write_text('int main(void) { return 0; }\n', encoding='utf-8')
    probe = base/'user-include.cmake'
    probe.write_text('message(STATUS "USER-PROJECT-INCLUDE ran")\n', encoding='utf-8')
    (src/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
set(PS3DK "{root.as_posix()}")
{prologue}
project(consumer C)
add_executable(app "${{CMAKE_SOURCE_DIR}}/main.c")
target_link_libraries(app PRIVATE gcm_cmd ps3dkprof_ext)
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/selected.txt" CONTENT
"$<TARGET_PROPERTY:app,LINK_LIBRARIES>\\n$<TARGET_PROPERTY:app,LINK_OPTIONS>\\n")
''', encoding='utf-8')
    cmd = [a.cmake, '-G', 'Unix Makefiles', '-S', str(src), '-B', str(base/'build'), *options]
    if user_include:
        cmd.append(f'-DCMAKE_PROJECT_INCLUDE={probe.as_posix()}')
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    log = r.stdout+r.stderr
    (base/'configure.log').write_text(log, encoding='utf-8')
    (base/'command.json').write_text(json.dumps(cmd), encoding='utf-8')
    sel = base/'build/selected.txt'
    return r.returncode, log, sel.read_text(encoding='utf-8') if sel.exists() else ''


LIVE_ANCHOR = '-Wl,-u,__ps3dk_prof_live_autostart_anchor'
AUTO_ANCHOR = '__ps3dk_prof_auto_startup_anchor'
# The toolchain file is read more than once in the top-level directory; the second read must keep the project's own
# entry and not include it twice.
TWICE = f'include("{REGISTER}")\ninclude("{REGISTER}")'
failed = 0


def check(name, ok, log):
    global failed
    print(f'{"PASS" if ok else "FAIL"} {name}')
    if not ok:
        print(log[-2500:])
        failed += 1


rc, log, sel = configure('live-and-auto', ['-DPS3_PROF_AUTO=ON', '-DPS3_PROF_LIVE_AUTOSTART=ON'], TWICE)
libs, opts = (sel.splitlines()+['', ''])[:2]
check('live-and-auto: configures, both modules ran, live archive mapped by the AUTO selection, user include once',
      rc == 0 and log.count('USER-PROJECT-INCLUDE ran') == 1 and "live viewer auto-start linked into 'app'" in log
      and 'libps3dkprof_auto.a' in libs and 'libgcm_cmd_prof_auto.a' in libs and 'libps3dkprof_ext_live.a' in libs
      and LIVE_ANCHOR in opts and AUTO_ANCHOR in opts
      and log.index('USER-PROJECT-INCLUDE ran') < log.index("live viewer auto-start linked into 'app'"), log)

rc, log, sel = configure('live-only', ['-DPS3_PROF_LIVE_AUTOSTART=ON'], TWICE)
libs, opts = (sel.splitlines()+['', ''])[:2]
check('live-only: live module only', rc == 0 and log.count('USER-PROJECT-INCLUDE ran') == 1
      and LIVE_ANCHOR in opts and AUTO_ANCHOR not in opts and 'libps3dkprof_auto.a' not in libs, log)

rc, log, sel = configure('auto-only', ['-DPS3_PROF_AUTO=ON'], TWICE)
libs, opts = (sel.splitlines()+['', ''])[:2]
check('auto-only: AUTO selector only', rc == 0 and log.count('USER-PROJECT-INCLUDE ran') == 1
      and AUTO_ANCHOR in opts and LIVE_ANCHOR not in opts and 'libps3dkprof_auto.a' in libs, log)

rc, log, sel = configure('off', [], TWICE)
check('off: no module, the project include untouched', rc == 0 and log.count('USER-PROJECT-INCLUDE ran') == 1
      and sel.splitlines()[0] == 'gcm_cmd;ps3dkprof_ext', log)

rc, log, sel = configure('no-user-include', ['-DPS3_PROF_AUTO=ON', '-DPS3_PROF_LIVE_AUTOSTART=ON'], TWICE,
                         user_include=False)
libs, opts = (sel.splitlines()+['', ''])[:2]
check('no-user-include: live and AUTO', rc == 0 and LIVE_ANCHOR in opts and AUTO_ANCHOR in opts
      and 'libps3dkprof_ext_live.a' in libs, log)

legacy = ('set(CMAKE_PROJECT_INCLUDE "{}" "{}")\nset(PS3_PROF_AUTO ON)\nset(PS3_PROF_LIVE_AUTOSTART ON)'
          .format((ROOT/'cmake/ps3-prof-live.cmake').as_posix(), (ROOT/'cmake/ps3-prof-auto.cmake').as_posix()))
rc, log, _ = configure('control-two-file-list', [], legacy, user_include=False)
want_fail = VERSION < (3, 29)
check(f'control: a two-file CMAKE_PROJECT_INCLUDE {"fails" if want_fail else "configures"} on this CMake',
      (rc != 0) == want_fail, log)

total = 6
print(f'prof-register cmake: {total - failed}/{total} passed')
raise SystemExit(1 if failed else 0)
