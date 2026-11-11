#!/usr/bin/env python3
"""Host refusal controls for the FIFO installed manifest and link providers."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('fifo_check', ROOT/'cmake/ps3-prof-fifo-check.py')
C = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(C)


class Checks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='fifo link ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.lib = self.root/'ppu/lib'
        self.inc = self.root/'ppu/include/ps3dk'
        self.lib.mkdir(parents=True)
        self.inc.mkdir(parents=True)
        for name in C.ARCHIVES:
            (self.lib/name).write_bytes(name.encode())
        for name in C.HEADERS:
            (self.inc/name).write_bytes(name.encode())
        self.manifest = dict(schema=1, kind='profiler-fifo-install', abi='ilp32',
            archives={n:C.sha(self.lib/n) for n in C.ARCHIVES},
            headers={n:C.sha(self.inc/n) for n in C.HEADERS})
        self.save()
        self.map = self.root/'link.map'
        self.providers = {s:str(self.lib/a)+'('+m+')' for s,(a,m) in C.REQUIRED.items()}
        self.write_map()

    def save(self):
        (self.lib/C.MANIFEST).write_text(json.dumps(self.manifest))

    def write_map(self, extra=''):
        self.map.write_text(extra+'\nCross Reference Table\nSymbol File\n'+
            '\n'.join(k+' '+v for k,v in self.providers.items())+'\n')

    def check(self):
        return C.check(self.root, 'ilp32', self.map)

    def test_valid_and_changed_hash(self):
        self.check()
        (self.lib/C.ARCHIVES[0]).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash'): self.check()

    def test_abi_and_missing_header(self):
        self.manifest['abi']='lp64';self.save()
        with self.assertRaisesRegex(ValueError, 'ABI'): self.check()
        self.manifest['abi']='ilp32';self.save()
        (self.inc/'prof_fifo.h').unlink()
        with self.assertRaises(OSError): self.check()

    def test_missing_and_foreign_provider(self):
        name='ps3tc_fifo_observe_install'
        del self.providers[name];self.write_map()
        with self.assertRaisesRegex(ValueError, 'provider'): self.check()
        self.providers[name]='foreign.a(fake.o)';self.write_map()
        with self.assertRaisesRegex(ValueError, 'provider'): self.check()

    def test_default_and_duplicate_provider(self):
        self.write_map('/sdk/libgcm_cmd.a(ps3tc_fifo_wrap.o)')
        with self.assertRaisesRegex(ValueError, 'default|mixed'): self.check()
        self.write_map()
        with self.map.open('a') as f:f.write('ps3dk_prof_init duplicate.o\n')
        with self.assertRaisesRegex(ValueError, 'repeated'): self.check()

    def test_mixed_composition_and_header_hash(self):
        self.write_map('/sdk/libps3dkprof_ext_func.a(func_hooks.o)')
        with self.assertRaisesRegex(ValueError, 'mixed'): self.check()
        self.write_map()
        (self.inc/'prof_gpu.h').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash'): self.check()

    def test_inventory_and_cross_reference(self):
        self.manifest['archives']['other.a']='0'*64;self.save()
        with self.assertRaisesRegex(ValueError, 'inventory'): self.check()
        del self.manifest['archives']['other.a'];self.save()
        self.map.write_text('no table')
        with self.assertRaisesRegex(ValueError, 'cross-reference'): self.check()

    def test_readelf_members(self):
        text='File: a.a(one.o)\nAttribute Section: gnu\nFile Attributes\n  Tag_GNU_Power_CellOS_Data_Model: ILP32\n'
        C.check_attributes(text, 'ilp32')
        for bad in [text.replace('ILP32','LP64'), text.replace('  Tag_GNU_Power_CellOS_Data_Model: ILP32\n',''), text+text, '']:
            with self.assertRaises(ValueError): C.check_attributes(bad, 'ilp32')


if __name__ == '__main__': unittest.main()
