#!/usr/bin/env python3
"""cmake/ps3-prof-live.cmake selection, configure only, with the host compiler: a profiler archive linked directly,
behind an INTERFACE library, behind a static wrapper and as $<LINK_ONLY:...> is found and the executable gets the
link request, net and sysmodule_stub; an executable without one is left alone; a project where no executable links
a profiler fails to configure; an extension link gets libps3dkprof_ext_live.a.
usage: live-autostart-cmake-test.py <new output dir>"""
import argparse
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('out', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
module = root / 'cmake/ps3-prof-live.cmake'
cmake = shutil.which('cmake') or 'cmake'

PROLOGUE = '''cmake_minimum_required(VERSION 3.21)
project(probe C)
file(WRITE "${CMAKE_BINARY_DIR}/m.c" "int main(void){return 0;}\\n")
file(WRITE "${CMAKE_BINARY_DIR}/l.c" "int lib_fn(void){return 0;}\\n")
'''
CASES = {
    'direct': ('add_executable(app "${CMAKE_BINARY_DIR}/m.c")\ntarget_link_libraries(app PRIVATE ps3dkprof)\n',
               0, ["linked into 'app'"], []),
    'interface': ('add_library(prof_if INTERFACE)\ntarget_link_libraries(prof_if INTERFACE ps3dkprof)\n'
                  'add_executable(app "${CMAKE_BINARY_DIR}/m.c")\ntarget_link_libraries(app PRIVATE prof_if)\n',
                  0, ["linked into 'app'"], []),
    'static-wrapper': ('add_library(wrap STATIC "${CMAKE_BINARY_DIR}/l.c")\n'
                       'target_link_libraries(wrap PRIVATE ps3prof)\n'
                       'add_library(wrap_alias ALIAS wrap)\n'
                       'add_executable(app "${CMAKE_BINARY_DIR}/m.c")\ntarget_link_libraries(app PRIVATE wrap_alias)\n',
                       0, ["linked into 'app'"], []),
    'mixed': ('add_executable(app "${CMAKE_BINARY_DIR}/m.c")\ntarget_link_libraries(app PRIVATE ps3dkprof)\n'
              'add_executable(tool "${CMAKE_BINARY_DIR}/m.c")\n',
              0, ["linked into 'app'", "'tool' links no profiler archive"], ["linked into 'tool'"]),
    'none': ('add_executable(tool "${CMAKE_BINARY_DIR}/m.c")\n', 1,
             ['no executable in this project links'], []),
    'ext': ('add_executable(app "${CMAKE_BINARY_DIR}/m.c")\n'
            'target_link_libraries(app PRIVATE "${PS3DK}/ppu/lib/libps3dkprof_ext.a")\n',
            0, ["linked into 'app'"], []),
}
EPILOGUE = '''
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _ps3_live_dump)
function(_ps3_live_dump)
  foreach(t app tool)
    if(TARGET ${t})
      get_target_property(l ${t} LINK_LIBRARIES)
      get_target_property(o ${t} LINK_OPTIONS)
      message(STATUS "DUMP ${t} libs=[${l}] opts=[${o}]")
    endif()
  endforeach()
endfunction()
'''
sdk = out / 'sdk'
for sub in ('ppu/lib', 'ppu/lib/lp64'):  # the host compiler's data model picks one (64-bit hosts: lp64)
    (sdk / sub).mkdir(parents=True)
    (sdk / sub / 'libps3dkprof_ext.a').write_bytes(b'!<arch>\n')
    (sdk / sub / 'libps3dkprof_ext_live.a').write_bytes(b'!<arch>\n')
failed = 0
for name, (body, want_rc, want, refuse) in CASES.items():
    src = out / name / 'src'
    src.mkdir(parents=True)
    # the dump is deferred after the module's own deferred step (registered later), so it sees the result
    (src / 'CMakeLists.txt').write_text(PROLOGUE + body + EPILOGUE)
    r = subprocess.run([cmake, '-S', str(src), '-B', str(out / name / 'build'), f'-DCMAKE_PROJECT_INCLUDE={module}',
                        f'-DPS3DK={sdk}'], capture_output=True, text=True)
    log = r.stdout + r.stderr
    (out / name / 'configure.log').write_text(log)
    ok = (r.returncode != 0) == bool(want_rc) and all(w in log for w in want) and not any(x in log for x in refuse)
    if ok and want_rc == 0 and name != 'mixed':
        dump = [ln for ln in log.splitlines() if 'DUMP app' in ln]
        ok = bool(dump) and 'net' in dump[0] and 'sysmodule_stub' in dump[0] and \
            '-Wl,-u,__ps3dk_prof_live_autostart_anchor' in dump[0]
        if name == 'ext':
            ok = ok and 'libps3dkprof_ext_live.a' in dump[0]
    print(f'{"PASS" if ok else "FAIL"} {name} rc={r.returncode}')
    if not ok:
        print(log[-1500:])
    failed += not ok
print(f'live-autostart cmake: {len(CASES) - failed}/{len(CASES)} passed')
raise SystemExit(1 if failed else 0)
