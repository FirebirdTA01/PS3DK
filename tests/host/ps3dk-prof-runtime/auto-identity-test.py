#!/usr/bin/env python3
"""Prelink identity controls with real host objects; no PPU or SELF execution."""
import argparse,importlib.util,json,subprocess,unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
MODULE=ROOT/'cmake/ps3-prof-auto-identity.py'
assert MODULE.is_file(),'AUTO_PRELINK_IDENTITY_IMPLEMENTATION'
spec=importlib.util.spec_from_file_location('auto_identity',MODULE)
I=importlib.util.module_from_spec(spec);spec.loader.exec_module(I)
OUT=None

class Identity(unittest.TestCase):
    def setUp(self):
        self.p=OUT/self._testMethodName;self.p.mkdir(parents=True)
        self.install=self.p/'install.json';self.install.write_text(json.dumps(dict(schema=1,abi='ilp32',
            archives={'libps3dkprof_auto.a':'1'*64,'libgcm_cmd_prof_auto.a':'2'*64})))
        self.main=self.p/'main.c';self.main.write_text('extern const unsigned char ps3dk_prof__auto_build_binding[32];\nint main(void){return ps3dk_prof__auto_build_binding[0]==0x100;}\n')
        self.obj=self.p/'main.o'
        subprocess.run(['cc','-c',str(self.main),'-o',str(self.obj)],check=True)
        self.elf=self.p/'linked';self.map=self.p/'linked.map'
        self.argv=['cc',str(self.obj),'-o',str(self.elf)]

    def prepare(self,argv=None):
        return I.prepare('ilp32',self.install,self.map,argv or self.argv)

    def test_repeat_and_input_change(self):
        a=self.prepare();b=self.prepare()
        self.assertEqual(a['binding'],b['binding'])
        self.assertEqual(a['mechanism'],b['mechanism'])
        self.obj.write_bytes(self.obj.read_bytes()+b'changed')
        c=self.prepare()
        self.assertNotEqual(a['binding'],c['binding'])
        self.assertEqual(a['mechanism'],c['mechanism'])

    def test_link_and_external_manifest(self):
        r=self.prepare();subprocess.run(r['command'],check=True)
        subprocess.run([str(self.elf)],check=True)
        result=I.finish(r,self.map)
        self.assertEqual(result['elf']['sha256'],I.sha(self.elf))
        self.assertEqual(result['binding'],r['binding'])
        self.assertFalse(result['packaged'])
        self.assertEqual(result['build_id'],r['binding'])
        obj=Path(r['identity_object'])
        self.assertIn(bytes.fromhex(r['binding']),obj.read_bytes())
        self.assertNotIn('self',result)

    def test_changed_input_and_object_refuse(self):
        r=self.prepare();subprocess.run(r['command'],check=True)
        original=self.obj.read_bytes();self.obj.write_bytes(original+b'x')
        with self.assertRaisesRegex(ValueError,'input changed'):I.finish(r,self.map)
        self.obj.write_bytes(original)
        Path(r['identity_object']).write_bytes(b'wrong')
        with self.assertRaisesRegex(ValueError,'identity object changed'):I.finish(r,self.map)

    def test_response_files_and_missing_inputs(self):
        rsp=self.p/'link.rsp';rsp.write_text('"'+str(self.obj)+'" -o "'+str(self.elf)+'"')
        r=self.prepare(['cc','@'+str(rsp)])
        self.assertIn(str(rsp.resolve()),r['inputs'])
        subprocess.run(r['command'],check=True)
        rsp.write_text('changed')
        with self.assertRaisesRegex(ValueError,'input changed'):I.finish(r,self.map)
        with self.assertRaisesRegex(ValueError,'missing input'):self.prepare(['cc',str(self.p/'missing.o'),'-o',str(self.elf)])
        rsp.write_text('@'+str(rsp))
        with self.assertRaisesRegex(ValueError,'response'):self.prepare(['cc','@'+str(rsp)])

    def test_existing_output_is_not_an_input(self):
        self.elf.write_bytes(b'previous build')
        r=self.prepare();self.assertNotIn(str(self.elf.resolve()),r['inputs'])
        self.elf.write_bytes(b'another previous build')
        self.assertEqual(r['binding'],self.prepare()['binding'])
        with self.assertRaisesRegex(ValueError,'one output'):self.prepare(['cc',str(self.obj)])
        with self.assertRaisesRegex(ValueError,'ABI'):I.prepare('ilp32',self.install,self.map,self.argv+['-mlp64'])

    def test_package_binds_exact_elf(self):
        r=self.prepare();subprocess.run(r['command'],check=True);I.finish(r,self.map)
        signed=self.p/'app.self';signed.write_bytes(b'signed fixture')
        packed=I.bind_self(self.map,signed)
        self.assertTrue(packed['packaged'])
        self.assertEqual(packed['self']['sha256'],I.sha(signed))
        self.elf.write_bytes(b'changed link')
        with self.assertRaisesRegex(ValueError,'ELF changed'):I.bind_self(self.map,signed)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True)
    args,rest=ap.parse_known_args();OUT=args.out.resolve();OUT.mkdir(parents=True,exist_ok=False)
    unittest.main(argv=[__file__,*rest])
