#!/usr/bin/env python3
"""Configure real CMake consumers; synthetic install bytes, no target claim."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('guard', ROOT/'cmake/ps3-prof-auto-check.py')
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)
OUT = None


class Selector(unittest.TestCase):
    def configure(self, name, enabled=True, libraries='', extra='', nested=False):
        base = OUT/name
        base.mkdir(parents=True, exist_ok=False)
        sdk = base/'sdk with space'
        lib, inc = GUARD.locations(sdk, 'lp64')
        lib.mkdir(parents=True)
        inc.mkdir(parents=True)
        for item in GUARD.ARCHIVES:
            (lib/item).write_bytes(item.encode())
        for item in GUARD.HEADERS:
            dest = inc/item
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(item.encode())
        manifest = dict(schema=1, kind='profiler-auto-install', abi='lp64',
                        source_commit='1'*40, automatic_attach=True,
                        archives={n:GUARD.sha(lib/n) for n in GUARD.ARCHIVES},
                        headers={n:GUARD.sha(inc/n) for n in GUARD.HEADERS})
        (lib/GUARD.MANIFEST).write_text(json.dumps(manifest), encoding='utf-8')
        source = base/'src'
        source.mkdir()
        (source/'main.c').write_text('int main(void) { return 0; }\n', encoding='utf-8')
        target_dir = source/'child' if nested else source
        target_dir.mkdir(exist_ok=True)
        target = '''add_executable(app "${CMAKE_SOURCE_DIR}/main.c")
target_link_libraries(app PRIVATE @LIBRARIES@)
@EXTRA@
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt" CONTENT
"$<TARGET_PROPERTY:app,LINK_LIBRARIES>\n$<TARGET_PROPERTY:app,COMPILE_DEFINITIONS>\n$<TARGET_PROPERTY:app,LINK_OPTIONS>\n$<TARGET_PROPERTY:app,C_LINKER_LAUNCHER>\n")
'''.replace('@LIBRARIES@', libraries).replace('@EXTRA@', extra)
        project = f'''cmake_minimum_required(VERSION 3.21)
set(PS3_PROF_AUTO {'ON' if enabled else 'OFF'})
set(PS3DK "{sdk.as_posix()}")
set(CMAKE_PROJECT_INCLUDE "{(ROOT/'cmake/ps3-prof-auto.cmake').as_posix()}")
project(consumer C)
'''
        if nested:
            (target_dir/'CMakeLists.txt').write_text(target, encoding='utf-8')
            project += 'add_subdirectory(child)\n'
        else:
            project += target
        (source/'CMakeLists.txt').write_text(project, encoding='utf-8')
        command = ['cmake', '-G', 'Unix Makefiles', '-S', str(source), '-B', str(base/'build')]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (base/'stdout.txt').write_text(result.stdout, encoding='utf-8')
        (base/'stderr.txt').write_text(result.stderr, encoding='utf-8')
        (base/'command.json').write_text(json.dumps(command), encoding='utf-8')
        selected = base/'build/selected.txt'
        return result, selected.read_text() if selected.exists() else ''

    def test_plain_consumer_and_subdirectory(self):
        for nested in (False, True):
            result, selected = self.configure('auto-'+str(nested), nested=nested)
            self.assertEqual(result.returncode, 0, result.stderr)
            for required in ('libps3dkprof_auto.a', 'libgcm_cmd_prof_auto.a',
                             'PS3TC_PROF_AUTO=1', '__ps3dk_prof_auto_startup_anchor',
                             'ps3dk_prof__auto_gcm_init_result', '--map', '--command'):
                self.assertIn(required, selected)

    def test_off_has_no_observer_or_provider_change(self):
        result, selected = self.configure('off', enabled=False, libraries='gcm_cmd ps3dkprof_ext')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(selected, 'gcm_cmd;ps3dkprof_ext\n\n\n\n')

    def test_normal_live_companion(self):
        result, selected = self.configure('live', libraries='gcm_cmd ps3dkprof_ext ps3dkprof_ext_live')
        self.assertEqual(result.returncode, 0, result.stderr)
        libs = selected.splitlines()[0]
        self.assertIn('libps3dkprof_ext_live.a', libs)
        self.assertNotIn(';gcm_cmd;', ';'+libs+';')
        self.assertNotIn(';ps3dkprof_ext;', ';'+libs+';')

    def test_conflicting_variants_and_launcher_refuse(self):
        for name, libs, extra in (
            ('waits', 'gcm_cmd ps3dkprof_ext_wait', ''),
            ('func', 'gcm_cmd ps3dkprof_ext_func', ''),
            ('fifo', 'gcm_cmd_prof_fifo', ''),
            ('launcher', 'gcm_cmd', 'set_property(TARGET app PROPERTY C_LINKER_LAUNCHER foreign)'),
        ):
            result, _ = self.configure(name, libraries=libs, extra=extra)
            self.assertNotEqual(result.returncode, 0, name+' unexpectedly selected')
            self.assertRegex(result.stderr, 'unsupported AUTO|existing link launcher')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args, rest = parser.parse_known_args()
    OUT = args.out.resolve()
    OUT.mkdir(parents=True, exist_ok=False)
    unittest.main(argv=[__file__]+rest)
