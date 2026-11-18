#!/usr/bin/env python3
"""PS3_PROF_AUTO_RING through the selector on the host: configure, build, link and run consumers of a stand-in AUTO
install (auto-cxx-link-test.py's, with the real prof.h / prof_gpu.h / prof_auto.h so the generated unit compiles).

OFF (the default): nothing generated, no --require-defined, the link is unchanged. ON: a generated constant unit (.c,
or .cpp in a CXX-only project) defines ps3dk_prof_automatic_ring_options with the configured sizes, the program reads
those values back; a reconfigure does not rewrite it. Invalid sizes are refused at configure with a message naming the
option. With the option ON and the definition removed from the generated unit the link fails, naming the symbol. No
PPU code, no target claim.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('cxxlink', Path(__file__).with_name('auto-cxx-link-test.py'))
CXXLINK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CXXLINK)
GUARD = CXXLINK.GUARD
OUT = None
SYMBOL = 'ps3dk_prof_automatic_ring_options'

C_APP = r'''
#include <stdio.h>
#include <ps3dk/prof_auto.h>
extern const ps3dk_prof_auto_ring_options ps3dk_prof_automatic_ring_options __attribute__((weak));
int main(void) {
    const ps3dk_prof_auto_ring_options *r = &ps3dk_prof_automatic_ring_options;
    if (!r) { puts("no ring constant"); return 0; }
    printf("ring %u %u %u %u\n", r->size, r->ring_blocks, r->ring_bytes, r->ring_records);
    return 0;
}
'''
CXX_APP = r'''
#include <cstdio>
#include <ps3dk/prof_auto.h>
extern "C" const ps3dk_prof_auto_ring_options ps3dk_prof_automatic_ring_options __attribute__((weak));
struct Probe { virtual ~Probe() {} virtual unsigned bytes() const { return ps3dk_prof_automatic_ring_options.ring_bytes; } };
int main() {
    if (!&ps3dk_prof_automatic_ring_options) { std::puts("no ring constant"); return 0; }
    const ps3dk_prof_auto_ring_options &r = ps3dk_prof_automatic_ring_options;
    Probe p;
    std::printf("ring %u %u %u %u\n", r.size, r.ring_blocks, p.bytes(), r.ring_records);
    return 0;
}
'''


def install(base):
    """The stand-in install, its three profiler headers replaced by the real ones and the manifest resealed."""
    sdk = base/'sdk'
    lib = CXXLINK.stand_in_install(sdk)
    inc = sdk/'ppu/include'
    for name, src in (('ps3dk/prof.h', ROOT/'sdk/include/ps3dk/prof.h'),
                      ('ps3dk/prof_auto.h', ROOT/'sdk/include/ps3dk/prof_auto.h'),
                      ('ps3dk/prof_gpu.h', ROOT/'sdk/libps3dkprof/src/rsx/prof_gpu.h')):
        shutil.copyfile(src, inc/name)
    manifest = json.loads((lib/GUARD.MANIFEST).read_text(encoding='utf-8'))
    manifest['headers'] = {n: GUARD.sha(inc/n) for n in GUARD.HEADERS}
    (lib/GUARD.MANIFEST).write_text(json.dumps(manifest), encoding='utf-8')
    return sdk


class RingOption(unittest.TestCase):
    def project(self, name, lang, options):
        base = OUT/name
        base.mkdir(parents=True)
        sdk = install(base)
        src = base/'src'
        src.mkdir()
        main = 'main.c' if lang == 'C' else 'main.cpp'
        (src/main).write_text(C_APP if lang == 'C' else CXX_APP, encoding='utf-8')
        forced = ','.join('-u,'+s for s in GUARD.REQUIRED)
        (src/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
set(PS3_PROF_AUTO ON)
set(PS3DK "{sdk.as_posix()}")
set(CMAKE_PROJECT_INCLUDE "{(ROOT/'cmake/ps3-prof-auto.cmake').as_posix()}")
project(ring_consumer {lang})
set(CMAKE_SIZEOF_VOID_P 4)
add_executable(app {main})
target_link_options(app PRIVATE "-Wl,{forced}")
''', encoding='utf-8')
        return base, src, base/'build', ['-D'+o for o in options]

    def step(self, base, tag, cmd, ok=True):
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        (base/f'{tag}.stdout').write_text(r.stdout)
        (base/f'{tag}.stderr').write_text(r.stderr)
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout[-3000:]+r.stderr[-3000:])
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout[-3000:]+r.stderr[-3000:])
        return r

    def configure(self, base, src, build, defs, ok=True):
        return self.step(base, 'configure', ['cmake', '-G', 'Unix Makefiles', '-S', str(src), '-B', str(build), *defs],
                         ok)

    def build_and_run(self, base, build):
        r = self.step(base, 'build', ['cmake', '--build', str(build), '--', 'VERBOSE=1'])
        self.assertIn('AUTO installed identity and providers PASS', r.stdout+r.stderr)
        return self.step(base, 'run', [str(build/'app')]).stdout

    def generated(self, build):
        return sorted(build.glob('app-auto-ring.*'))

    def test_off_is_unchanged(self):
        base, src, build, defs = self.project('off', 'C', [])
        self.configure(base, src, build, defs)
        self.assertEqual(self.generated(build), [])
        self.assertEqual(self.build_and_run(base, build), 'no ring constant\n')
        self.assertNotIn('--require-defined', (build/'CMakeFiles/app.dir/link.txt').read_text())
        # The app's weak reference stays unresolved: no generated object is linked and nothing defines the constant.
        # (Match the object and symbol names, not 'auto-ring': the test's own output directory contains that text.)
        amap = (build/'app-auto.map').read_text()
        self.assertNotIn('app-auto-ring', amap)
        # ld's cross-reference table lists the symbol with the app's own object as its only (weak) referrer.
        xref = [line.split() for line in amap.splitlines() if line.startswith(SYMBOL)]
        self.assertTrue(all(len(f) == 2 and f[1].endswith('main.c.o') for f in xref), xref)
        print('OFF: nothing generated, no required symbol, no ring constant')

    def check_on(self, name, lang, options, expect):
        base, src, build, defs = self.project(name, lang, ['PS3_PROF_AUTO_RING=ON']+options)
        self.configure(base, src, build, defs)
        unit = self.generated(build)
        self.assertEqual([p.suffix for p in unit], ['.c' if lang == 'C' else '.cpp'])
        text = unit[0].read_text()
        self.assertIn('PS3DK_PROF_AUTO_RING_OPTIONS_INIT(%du, %du)' % expect[1:], text)
        self.assertIn('-Wl,--require-defined,'+SYMBOL, (build/'CMakeFiles/app.dir/link.txt').read_text())
        out = self.build_and_run(base, build)
        self.assertEqual(out, 'ring 16 %d %d %d\n' % expect)
        defined = [line for line in (build/'app-auto.map').read_text().splitlines() if line.startswith(SYMBOL)]
        self.assertTrue(defined and 'app-auto-ring' in defined[0], defined)
        stamp = unit[0].stat().st_mtime_ns
        self.configure(base, src, build, defs)
        self.assertEqual(unit[0].stat().st_mtime_ns, stamp, 'reconfigure rewrote the generated unit')
        print('ON %s %s: %s defines %s = %s' % (lang, options or 'defaults', unit[0].name, SYMBOL, out.strip()))
        return base, build, unit[0]

    def test_on_c_defaults(self):
        self.check_on('on-c', 'C', [], (2, 16 << 20, 16384))

    def test_on_cxx_only_small(self):
        self.check_on('on-cxx', 'CXX', ['PS3_PROF_AUTO_RING_MB=2', 'PS3_PROF_AUTO_RING_RECORDS=4096'],
                      (2, 2 << 20, 4096))

    def test_invalid_sizes_refused_at_configure(self):
        cases = [('PS3_PROF_AUTO_RING_MB', v) for v in ('0', '257', '1000', 'abc', '1.5', '016', '-1', '')] + \
                [('PS3_PROF_AUTO_RING_RECORDS', v) for v in ('0', '65537', '100000', 'x', '')]
        for i, (option, value) in enumerate(cases):
            base, src, build, defs = self.project(f'invalid{i}', 'C', ['PS3_PROF_AUTO_RING=ON', f'{option}={value}'])
            r = self.configure(base, src, build, defs, ok=False)
            self.assertIn(option+' must be', r.stdout+r.stderr)
            self.assertEqual(self.generated(build), [])
        print('invalid sizes refused at configure: %d cases' % len(cases))

    def test_missing_definition_is_a_link_error(self):
        for lang in ('C', 'CXX'):
            base, src, build, defs = self.project('missing-'+lang.lower(), lang, ['PS3_PROF_AUTO_RING=ON'])
            self.configure(base, src, build, defs)
            unit = self.generated(build)[0]
            unit.write_text('/* the definition removed */\n')
            r = self.step(base, 'build', ['cmake', '--build', str(build), '--', 'VERBOSE=1'], ok=False)
            self.assertRegex(r.stdout+r.stderr, r"required symbol .%s. not defined" % SYMBOL)
            self.assertFalse((build/'app').exists())
        print('ON without the definition: link error naming %s (C and CXX)' % SYMBOL)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args, rest = parser.parse_known_args()
    OUT = args.out.resolve()
    OUT.mkdir(parents=True, exist_ok=False)
    unittest.main(argv=[__file__]+rest, verbosity=2)
