#!/usr/bin/env python3
"""Exercise header ownership through the real build script, without compilers.

The small upstream fixture models the pinned PSL1GHT install dispatch and its
recursive PPU header copy. Runtime compilation/generation alone is stubbed.
Restoring that unfiltered copy must overwrite our wrappers and fail this test.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
OWNED = ('sys/mutex.h', 'sys/cond.h', 'sys/event_queue.h', 'sys/sem.h',
         'sys/systime.h', 'sys/thread.h', 'sys/process.h',
         'sys/synchronization.h', 'sys/lv2_fs_ext.h', 'sys/spu.h', 'ppu-asm.h')


class InstallOrderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='psl1ght-install-')
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.repo = self.work / 'repo'
        self.source = self.repo / 'src/ps3dev/PSL1GHT'
        self.prefix = self.work / 'stage'
        self.sdk = self.prefix / 'ps3dk'
        self.scripts = self.repo / 'scripts'
        self.scripts.mkdir(parents=True)
        for name in ['env.sh', 'build-psl1ght.sh', 'install-psl1ght.sh']:
            source = ROOT / 'scripts' / name
            if source.is_file():
                shutil.copy2(source, self.scripts / name)

        # These are the expensive dependencies. No compiler is invoked: the
        # stand-in accepts only the host make_sprx output operation.
        self.write('scripts/gen-make-sprx-source.sh',
                   '#!/bin/bash\nset -eu\nmkdir -p "$(dirname "$2")"\nprintf "fixture\\n" > "$2"\n', executable=True)
        self.write('scripts/build-runtime-lv2.sh',
                   '#!/bin/bash\nset -eu\nprintf "runtime override\\n" > "$PS3DEV/runtime-installed"\n', executable=True)
        self.write('scripts/host-compiler',
                   '#!/bin/bash\nset -eu\nwhile [ "$#" -gt 0 ]; do\n'
                   '  if [ "$1" = -o ]; then printf "fixture host tool\\n" > "$2"; exit; fi\n'
                   '  shift\ndone\nexit 1\n', executable=True)
        for relative in ['ppu/bin/powerpc64-ps3-elf-gcc', 'spu/bin/spu-elf-gcc']:
            path = self.prefix / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#!/bin/sh\necho "unexpected target compiler invocation" >&2\nexit 99\n')
            path.chmod(0o755)

        self.upstream('Makefile', '.PHONY: all install install-ctrl\n'
                      'all:\n\ttest -f "$(PSL1GHT)/ppu_rules"\n'
                      'install-ctrl:\n\tmkdir -p "$(PSL1GHT)"\n'
                      '\tcp ppu_rules "$(PSL1GHT)/ppu_rules"\n'
                      'install:\n\t$(MAKE) -C ppu install\n\t$(MAKE) -C spu install\n'
                      '\t$(MAKE) -C common install\n\t$(MAKE) -C tools install\n')
        self.upstream('ppu_rules', 'fixture rules\n')
        for header in OWNED:
            self.upstream('ppu/include/' + header, '/* obsolete upstream ' + header + ' */\n')
        self.upstream('ppu/include/lv2/keep.h', 'unrelated LV2 header\n')
        self.upstream('ppu/include/sys/keep.h', 'unrelated sys header\n')
        self.upstream('ppu/include/lv2/mutex.h', 'upstream legacy mutex header\n')
        self.upstream('ppu/include/lv2/cond.h', 'upstream legacy condition header\n')
        self.upstream('ppu/include/other/mutex.h', 'unrelated mutex header\n')
        self.upstream('ppu/include/keep.h', 'unrelated PPU header\n')
        self.upstream('ppu/Makefile', '.PHONY: install install-headers\n'
                      'install-headers:\n\tmkdir -p "$(PSL1GHT)/ppu"\n'
                      '\tcp -R include "$(PSL1GHT)/ppu/"\n'
                      'install: install-headers\n\t$(MAKE) -C runtime install\n')
        self.upstream('ppu/runtime/Makefile', '.PHONY: install\ninstall:\n'
                      '\tmkdir -p "$(PSL1GHT)/ppu/lib"\n'
                      '\tcp runtime.a "$(PSL1GHT)/ppu/lib/fixture.a"\n')
        self.upstream('ppu/runtime/runtime.a', 'fixture runtime archive\n')
        self.upstream('spu/include/keep.h', 'unrelated SPU header\n')
        self.upstream('spu/Makefile', '.PHONY: install install-headers\n'
                      'install-headers:\n\tmkdir -p "$(PSL1GHT)/spu"\n'
                      '\tcp -R include "$(PSL1GHT)/spu/"\n'
                      'install: install-headers\n\tmkdir -p "$(PSL1GHT)/spu/lib"\n'
                      '\tcp runtime.a "$(PSL1GHT)/spu/lib/fixture.a"\n')
        self.upstream('spu/runtime.a', 'fixture SPU archive\n')
        self.upstream('common/Makefile', '.PHONY: install\ninstall:\n'
                      '\tmkdir -p "$(PSL1GHT)/ppu/include/common"\n'
                      '\tcp keep.h "$(PSL1GHT)/ppu/include/common/keep.h"\n')
        self.upstream('common/keep.h', 'unrelated common header\n')
        self.upstream('tools/Makefile', '.PHONY: install\ninstall:\n'
                      '\tmkdir -p "$(PS3DEV)/bin"\n'
                      '\tcp fixture-tool "$(PS3DEV)/bin/fixture-tool"\n')
        self.upstream('tools/fixture-tool', 'fixture tool\n')
        self.env = {key: os.environ[key] for key in ['HOME', 'PATH', 'LANG'] if key in os.environ}
        self.env.update(PS3DEV=str(self.prefix), PS3_BUILD_ROOT=str(self.work / 'build'),
                        CC=str(self.scripts / 'host-compiler'))

    def write(self, relative, text, executable=False):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if executable:
            path.chmod(0o755)

    def upstream(self, relative, text):
        self.write('src/ps3dev/PSL1GHT/' + relative, text)

    def install_sdk_headers(self):
        # Invoke the SDK's real install rule, restricted to the owned headers;
        # this stays tiny and never copies an installed SDK or builds a library.
        proc = subprocess.run(['make', '-s', '-C', str(ROOT / 'sdk'), 'install-headers',
                               'HEADERS=' + ' '.join('include/' + name for name in OWNED),
                               'PS3DEV=' + str(self.prefix), 'PS3DK=' + str(self.sdk)],
                              env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def run_install(self, expect=0):
        proc = subprocess.run(['bash', str(self.scripts / 'build-psl1ght.sh')],
                              env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(proc.returncode, expect, proc.stdout + proc.stderr)
        return proc

    def assert_other_payload(self):
        expected = {
            'ps3dk/ppu/include/keep.h': 'unrelated PPU header\n',
            'ps3dk/ppu/include/lv2/keep.h': 'unrelated LV2 header\n',
            'ps3dk/ppu/include/sys/keep.h': 'unrelated sys header\n',
            'ps3dk/ppu/include/lv2/mutex.h': 'upstream legacy mutex header\n',
            'ps3dk/ppu/include/lv2/cond.h': 'upstream legacy condition header\n',
            'ps3dk/ppu/include/other/mutex.h': 'unrelated mutex header\n',
            'ps3dk/ppu/lib/fixture.a': 'fixture runtime archive\n',
            'ps3dk/spu/include/keep.h': 'unrelated SPU header\n',
            'ps3dk/spu/lib/fixture.a': 'fixture SPU archive\n',
            'ps3dk/ppu/include/common/keep.h': 'unrelated common header\n',
            'ps3dk/ppu_rules': 'fixture rules\n',
            'bin/fixture-tool': 'fixture tool\n',
            'runtime-installed': 'runtime override\n',
        }
        for relative, content in expected.items():
            self.assertTrue((self.prefix / relative).is_file(), 'missing install payload: ' + relative)
            self.assertEqual((self.prefix / relative).read_text(), content, relative)

    def test_psl1ght_reinstall_preserves_sdk_owned_headers_and_other_payload(self):
        self.install_sdk_headers()
        before = {str(p.relative_to(self.source)): p.read_bytes()
                  for p in self.source.rglob('*') if p.is_file()}
        for _ in range(2):
            self.run_install()
            for header in OWNED:
                self.assertEqual((self.sdk / 'ppu/include' / header).read_bytes(),
                                 (ROOT / 'sdk/include' / header).read_bytes(),
                                 'PSL1GHT overwrote SDK-owned ' + header)
            self.assert_other_payload()
        after = {str(p.relative_to(self.source)): p.read_bytes()
                 for p in self.source.rglob('*') if p.is_file()}
        self.assertEqual(after, before, 'install must not edit the upstream source tree')

    def test_first_psl1ght_install_defers_only_owned_headers_to_sdk(self):
        self.run_install()
        for header in OWNED:
            self.assertFalse((self.sdk / 'ppu/include' / header).exists(), header)
        self.assert_other_payload()
        self.install_sdk_headers()
        for header in OWNED:
            self.assertEqual((self.sdk / 'ppu/include' / header).read_bytes(),
                             (ROOT / 'sdk/include' / header).read_bytes())

    def test_runtime_install_failure_does_not_continue_with_other_components(self):
        self.upstream('ppu/runtime/Makefile', '.PHONY: install\ninstall:\n\tfalse\n')
        self.run_install(expect=2)
        self.assertFalse((self.sdk / 'spu/include/keep.h').exists())
        self.assertFalse((self.prefix / 'bin/fixture-tool').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
