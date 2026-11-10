#!/usr/bin/env python3
"""Host refusal controls for the AUTO installed manifest and link providers."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('auto_check', ROOT/'cmake/ps3-prof-auto-check.py')
assert (ROOT/'cmake/ps3-prof-auto-check.py').is_file(), 'AUTO_LINK_GUARD_IMPLEMENTATION'
C = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(C)


class Checks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='auto link ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.lib = self.root/'ppu/lib'
        self.inc = self.root/'ppu/include'
        self.lib.mkdir(parents=True)
        self.inc.mkdir(parents=True)
        for name in C.ARCHIVES:
            (self.lib/name).write_bytes(name.encode())
        for name in C.HEADERS:
            (self.inc/name).parent.mkdir(parents=True,exist_ok=True)
            (self.inc/name).write_bytes(name.encode())
        self.manifest = dict(schema=1, kind='profiler-auto-install', abi='ilp32', source_commit='1'*40, automatic_attach=True,
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
        (self.inc/'ps3dk/prof_auto.h').unlink()
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
        (self.inc/'cell/gcm.h').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash'): self.check()

    def test_inventory_and_cross_reference(self):
        self.manifest['archives']['other.a']='0'*64;self.save()
        with self.assertRaisesRegex(ValueError, 'inventory'): self.check()
        del self.manifest['archives']['other.a'];self.save()
        self.map.write_text('no table')
        with self.assertRaisesRegex(ValueError, 'cross-reference'): self.check()

    def test_startup_and_automatic_init_are_required(self):
        required={'__ps3dk_prof_auto_startup_anchor', 'ps3dk_prof__auto_gcm_init_result',
                  'ps3dk_prof__auto_begin', 'ps3dk_prof__auto_end', 'rsxDrawVertexArray',
                  'rsxDrawIndexArray', 'rsxClearSurface', 'ps3dk_prof__auto_encode_ops',
                  'ps3dk_prof__auto_queue_claim', 'ps3dk_prof__auto_publish_before',
                  'ps3dk_prof__auto_publish_after', 'ps3dk_prof__auto_completion_read',
                  'ps3dk_prof__auto_owner_retire', 'rsxFlushBuffer',
                  'ps3dk_prof__auto_frame', 'ps3dk_prof_auto_frame_end', 'ps3dk_prof__auto_frames_close',
                  'ps3dk_prof__auto_clock_sample', 'ps3dk_prof__auto_clock_before', 'ps3dk_prof__auto_clock_after'}
        required.update({'ps3dk_prof__auto_fifo_boundary', 'ps3dk_prof__auto_lease_owner',
                         'ps3dk_prof__fifo_auto_prepare', 'ps3dk_prof__fifo_auto_publish',
                         'ps3dk_prof__fifo_auto_cancel'})
        self.assertTrue(required <= C.REQUIRED.keys())
        for symbol in required:
            old=self.providers.pop(symbol); self.write_map()
            with self.assertRaisesRegex(ValueError,'provider'): self.check()
            self.providers[symbol]=old
        self.manifest['automatic_attach']=False; self.save()
        with self.assertRaisesRegex(ValueError,'automatic attach'): self.check()

    def test_readelf_members(self):
        text='File: a.a(one.o)\nAttribute Section: gnu\nFile Attributes\n  Tag_GNU_Power_CellOS_Data_Model: ILP32\n'
        C.check_attributes(text, 'ilp32')
        for bad in [text.replace('ILP32','LP64'), text.replace('  Tag_GNU_Power_CellOS_Data_Model: ILP32\n',''), text+text, '']:
            with self.assertRaises(ValueError): C.check_attributes(bad, 'ilp32')


if __name__ == '__main__': unittest.main()
