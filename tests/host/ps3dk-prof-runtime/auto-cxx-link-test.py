#!/usr/bin/env python3
"""A C++ PS3_PROF_AUTO consumer configured, built and linked on the host through the selector and its link guard.

The installed AUTO providers are stand-in host archives whose members define every symbol the guard requires,
sealed into a prof-auto.json like the real install. The consumer has a class with a constructor and a virtual
destructor (C1/C2, D0/D1/D2 variants), a replaced operator new and a template instantiation, so a demangled cross
reference table repeats names. The link must pass the guard (the selector links with --no-demangle). No PPU code,
no target claim. CMAKE_SIZEOF_VOID_P is set to 4 after project() so the host stands in for the ILP32 data model.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('guard', ROOT/'cmake/ps3-prof-auto-check.py')
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)
OUT = None

APP = r'''
#include <cstdio>
#include <cstdlib>
#include <new>
class AlphaUIAnimation {
public:
    explicit AlphaUIAnimation(int frames) : frames_(frames) {}
    virtual ~AlphaUIAnimation() { std::printf("done %d\n", frames_); }
    virtual int step() { return --frames_; }
private:
    int frames_;
};
template <typename T, unsigned N> struct Pool {
    T *items[N] = {};
    void release(T *item) { for (auto &slot : items) if (slot == item) slot = nullptr; delete item; }
};
template struct Pool<AlphaUIAnimation, 16u>;
void *operator new(std::size_t size) { void *p = std::malloc(size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
int main() {
    Pool<AlphaUIAnimation, 16u> pool;
    AlphaUIAnimation *a = new AlphaUIAnimation(3);
    pool.items[0] = a;
    int left = a->step();
    pool.release(a);
    return left == 2 ? 0 : 1;
}
'''


def stand_in_install(sdk):
    """Host archives with one member per REQUIRED member, sealed like an install."""
    lib, inc = GUARD.locations(sdk, 'ilp32')
    lib.mkdir(parents=True)
    inc.mkdir(parents=True)
    selector = (ROOT/'cmake/ps3-prof-auto.cmake').read_text()
    forced = set(re.findall(r'-u,([A-Za-z_][A-Za-z0-9_]*)', selector))
    members = {}
    for symbol, (archive, member) in GUARD.REQUIRED.items():
        members.setdefault((archive, member), []).append(symbol)
    for symbol in sorted(forced - GUARD.REQUIRED.keys()):
        members.setdefault((GUARD.ARCHIVES[0], 'auto_runtime.o'), []).append(symbol)
    build = sdk/'members'
    build.mkdir()
    by_archive = {a: [] for a in GUARD.ARCHIVES}
    for (archive, member), symbols in members.items():
        stem = archive[:-2]+'-'+member[:-2]
        src = build/(stem+'.c')
        src.write_text(''.join('void %s(void) {}\n' % s for s in symbols))
        obj = build/archive[:-2]/member
        obj.parent.mkdir(exist_ok=True)
        subprocess.run(['cc', '-c', str(src), '-o', str(obj)], check=True)
        by_archive[archive].append(obj)
    live = build/'live.c'
    live.write_text('void ps3dk_prof__live_stand_in(void) {}\n')
    subprocess.run(['cc', '-c', str(live), '-o', str(build/'live.o')], check=True)
    by_archive['libps3dkprof_ext_live.a'].append(build/'live.o')
    for archive, objs in by_archive.items():
        subprocess.run(['ar', 'rcsD', str(lib/archive), *map(str, objs)], check=True)
    for item in GUARD.HEADERS:
        dest = inc/item
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(item.encode())
    manifest = dict(schema=1, kind='profiler-auto-install', abi='ilp32', source_commit='1'*40, automatic_attach=True,
                    archives={n: GUARD.sha(lib/n) for n in GUARD.ARCHIVES},
                    headers={n: GUARD.sha(inc/n) for n in GUARD.HEADERS})
    (lib/GUARD.MANIFEST).write_text(json.dumps(manifest), encoding='utf-8')
    return lib


class CxxLink(unittest.TestCase):
    def test_cxx_consumer_links_through_the_guard(self):
        base = OUT/'cxx'
        base.mkdir(parents=True)
        sdk = base/'sdk'
        lib = stand_in_install(sdk)
        src = base/'src'
        src.mkdir()
        (src/'main.cpp').write_text(APP, encoding='utf-8')
        forced = ','.join('-u,'+s for s in GUARD.REQUIRED)
        (src/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
set(PS3_PROF_AUTO ON)
set(PS3DK "{sdk.as_posix()}")
set(CMAKE_PROJECT_INCLUDE "{(ROOT/'cmake/ps3-prof-auto.cmake').as_posix()}")
project(cxx_consumer CXX)
set(CMAKE_SIZEOF_VOID_P 4)
add_executable(app main.cpp)
target_link_options(app PRIVATE "-Wl,{forced}")
''', encoding='utf-8')
        build = base/'build'
        steps = [['cmake', '-G', 'Unix Makefiles', '-S', str(src), '-B', str(build)],
                 ['cmake', '--build', str(build), '--', 'VERBOSE=1']]
        for i, cmd in enumerate(steps):
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            (base/f'step{i}.stdout').write_text(r.stdout)
            (base/f'step{i}.stderr').write_text(r.stderr)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:]+r.stderr[-3000:])
        self.assertIn('AUTO installed identity and providers PASS', r.stdout+r.stderr)
        self.assertIn('--no-demangle', (build/'CMakeFiles/app.dir/link.txt').read_text())
        mapfile = build/'app-auto.map'
        table = mapfile.read_text().split('Cross Reference Table', 1)[1]
        names = [s for s, _ in GUARD.cross_references(table)]
        # mangled: the constructor/destructor variants, operator new and the template stay distinct
        for mangled in ('_ZN16AlphaUIAnimationD1Ev', '_ZN16AlphaUIAnimationD2Ev', '_ZN16AlphaUIAnimationC2Ei',
                        '_Znwm', '_ZN4PoolI16AlphaUIAnimationLj16EE7releaseEPS0_'):
            self.assertIn(mangled, names)
        self.assertEqual(len(names), len(set(names)))
        providers = json.loads(mapfile.with_name(mapfile.name+'.auto.json').read_text())
        self.assertEqual(providers['ps3dk_prof__auto_begin'], str(lib/'libps3dkprof_auto.a')+'(auto_runtime.o)')
        r = subprocess.run([str(build/'app')], capture_output=True, text=True, timeout=30)
        self.assertEqual((r.returncode, r.stdout), (0, 'done 2\n'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args, rest = parser.parse_known_args()
    OUT = args.out.resolve()
    OUT.mkdir(parents=True, exist_ok=False)
    unittest.main(argv=[__file__]+rest)
