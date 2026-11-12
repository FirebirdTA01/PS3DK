#!/usr/bin/env python3
"""Prelink identity controls with real host objects; no PPU or SELF execution.

--mutant compile-as-driver drops the explicit '-x c' from the identity compile; --mutant internal-definitions drops
the extern declarations from the generated source. Each must fail the C++-driver controls. --mutant
positional-inputs binds only the positional files named on the command line (the reviewed defect: a changed -l
library kept the binding); it must fail the traced-input controls.
"""
import argparse,importlib.util,json,os,shutil,subprocess,sys,unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
MODULE=ROOT/'cmake/ps3-prof-auto-identity.py'
assert MODULE.is_file(),'AUTO_PRELINK_IDENTITY_IMPLEMENTATION'
OUT=None
MUTANT=None
SYMBOLS=('ps3dk_prof__auto_mechanism','ps3dk_prof__auto_build_binding','ps3dk_prof__auto_build_id',
         'ps3dk_prof__auto_app')


def load(mutant):
    code=MODULE.read_text()
    if mutant=='compile-as-driver':
        old="'-x', 'c', '-c', str(source)";assert code.count(old)==1;code=code.replace(old,"'-c', str(source)")
    elif mutant=='internal-definitions':
        old="decls+'\\n'+";assert code.count(old)==2;code=code.replace(old,'')
    elif mutant=='positional-inputs':
        old="traced = _traced_inputs(argv, trial_obj, cwd)";assert code.count(old)==1
        code=code.replace(old,"_traced_inputs(argv, trial_obj, cwd); traced = set(files)")
    module=type(sys)('auto_identity');module.__file__=str(MODULE)
    exec(compile(code,str(MODULE),'exec'),module.__dict__)
    return module


def globals_of(nm,obj):
    """{symbol: nm type letter} for the four identity symbols, demangled names included."""
    out=subprocess.run([nm,'-C',str(obj)],capture_output=True,text=True,check=True).stdout
    found={}
    for line in out.splitlines():
        parts=line.split()
        if len(parts)>=3 and parts[2].split('[')[0] in SYMBOLS:found[parts[2].split('[')[0]]=parts[1]
    return found

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

    def cxx_driver(self):
        """A g++-style driver name that records each command line and runs the host g++."""
        log=self.p/'driver.log';d=self.p/'bin';d.mkdir()
        wrapper=d/'g++'
        wrapper.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "'+str(log)+'"\nexec '+shutil.which('g++')+' "$@"\n')
        wrapper.chmod(0o755)
        return wrapper,log

    def test_cxx_driver_compiles_identity_as_c(self):
        # Review: a project that links with g++ compiled the generated .c as C++, where namespace-scope const arrays
        # have internal linkage; the C-declared references then stayed undefined at the link.
        wrapper,log=self.cxx_driver()
        r=self.prepare([str(wrapper),str(self.obj),'-o',str(self.elf)])
        compile_line=[l for l in log.read_text().splitlines() if '.identity.c' in l]
        self.assertEqual(len(compile_line),1,log.read_text())
        words=compile_line[0].split()
        src=words.index(str(self.map)+'.identity.c')
        self.assertEqual(words[src-3:src],['-x','c','-c'],compile_line[0])
        types=globals_of('nm',r['identity_object'])
        self.assertEqual(sorted(types),sorted(SYMBOLS),types)
        self.assertTrue(all(t in 'RD' for t in types.values()),types)
        subprocess.run(r['command'],check=True)
        subprocess.run([str(self.elf)],check=True)

    def test_generated_source_is_external_as_cxx(self):
        # The object must be right even if a driver compiles the source as C++ regardless.
        self.prepare()
        cxx=self.p/'as-cxx.o'
        subprocess.run(['g++','-x','c++','-c',str(self.map)+'.identity.c','-o',str(cxx)],check=True)
        types=globals_of('nm',cxx)
        self.assertEqual(sorted(types),sorted(SYMBOLS),types)
        self.assertTrue(all(t in 'RD' for t in types.values()),types)
        # with a PPU C++ compiler on hand, the same for the target
        ppu=shutil.which('powerpc64-ps3-elf-g++') or (os.environ.get('PS3DEV') and
            shutil.which('powerpc64-ps3-elf-g++',path=os.environ['PS3DEV']+'/ppu/bin'))
        if ppu:
            obj=self.p/'ppu-as-cxx.o'
            subprocess.run([ppu,'-x','c++','-c',str(self.map)+'.identity.c','-o',str(obj)],check=True)
            types=globals_of(ppu[:-3]+'nm' if ppu.endswith('g++') else 'nm',obj)
            self.assertTrue(len(types)==4 and all(t in 'RD' for t in types.values()),types)
            print('PPU nm:',types,file=sys.stderr)

    def answer_lib(self,value):
        """libanswer.a in its own directory, answer() returning value."""
        d=self.p/'lib';d.mkdir(exist_ok=True)
        (d/'a.c').write_text('int answer(void){return '+str(value)+';}\n')
        subprocess.run(['cc','-c',str(d/'a.c'),'-o',str(d/'a.o')],check=True)
        lib=d/'libanswer.a'
        if lib.exists():lib.unlink()
        subprocess.run(['ar','rcs',str(lib),str(d/'a.o')],check=True)
        return d

    def answer_main(self):
        src=self.p/'amain.c';src.write_text('int answer(void);\nint main(void){return answer();}\n')
        obj=self.p/'amain.o';subprocess.run(['cc','-c',str(src),'-o',str(obj)],check=True)
        return obj

    def test_library_change_changes_binding(self):
        # Review 2026-11-12: a different -l archive with the same main.o and argv kept the binding and build_id.
        d=self.answer_lib(1);obj=self.answer_main()
        argv=['cc',str(obj),'-L',str(d),'-lanswer','-o',str(self.elf)]
        a=self.prepare(argv)
        self.assertIn(str((d/'libanswer.a').resolve()),a['inputs'])
        subprocess.run(a['command'],check=True)
        self.assertEqual(subprocess.run([str(self.elf)]).returncode,1)
        self.answer_lib(2)
        b=self.prepare(argv)
        self.assertNotEqual(a['binding'],b['binding'])
        subprocess.run(b['command'],check=True)
        self.assertEqual(subprocess.run([str(self.elf)]).returncode,2)
        # a library swapped between prepare and finish is refused as a changed input
        self.answer_lib(3)
        with self.assertRaisesRegex(ValueError,'input changed'):I.finish(b,self.map)

    def test_linker_script_inputs_are_bound(self):
        # A script given to the linker, and the object its INPUT() names, are both inputs.
        extra_c=self.p/'extra.c';extra_c.write_text('int extra_sym = 1;\n')
        extra=self.p/'extra.o';subprocess.run(['cc','-c',str(extra_c),'-o',str(extra)],check=True)
        script=self.p/'extra.ld';script.write_text('INPUT('+str(extra)+')\n')
        argv=['cc',str(self.obj),'-Wl,'+str(script),'-o',str(self.elf)]
        a=self.prepare(argv)
        self.assertIn(str(script.resolve()),a['inputs']);self.assertIn(str(extra.resolve()),a['inputs'])
        extra_c.write_text('int extra_sym = 2;\n');subprocess.run(['cc','-c',str(extra_c),'-o',str(extra)],check=True)
        self.assertNotEqual(a['binding'],self.prepare(argv)['binding'])

    def test_driver_default_inputs_are_bound(self):
        # The driver's own startup objects and libraries never appear on the command line; they are still inputs.
        names=[Path(p).name for p in self.prepare()['inputs']]
        self.assertTrue(any(n.startswith(('crt','Scrt')) and n.endswith('.o') for n in names),names)
        self.assertTrue(any(n.startswith('libc') or n.startswith('libgcc') for n in names),names)
        self.assertNotIn(Path(str(self.map)+'.identity-trial.o').name,names)

    def test_unresolved_forms_refuse(self):
        rsp=self.p/'ld.rsp';rsp.write_text(str(self.obj))
        for extra in (['-Wl,@'+str(rsp)],['-Xlinker','@'+str(rsp)],['-specs='+str(self.p/'missing.specs')]):
            with self.assertRaisesRegex(ValueError,'unresolved link input'):self.prepare(self.argv+extra)
        with self.assertRaisesRegex(ValueError,'trial link failed'):
            self.prepare(['cc',str(self.obj),'-lno-such-library-here','-o',str(self.elf)])
        specs=self.p/'empty.specs';specs.write_text('\n')
        self.assertIn(str(specs.resolve()),self.prepare(self.argv+['-specs='+str(specs)])['inputs'])

    def test_map_loads_must_be_bound(self):
        argv=self.argv+['-Wl,-Map,'+str(self.map)]
        r=self.prepare(argv);subprocess.run(r['command'],check=True)
        I.finish(r,self.map,require_map=True)
        with self.map.open('a') as f:f.write('LOAD '+str(self.p/'unbound.o')+'\n')
        with self.assertRaisesRegex(ValueError,'not in the binding'):I.finish(r,self.map,require_map=True)
        self.map.unlink()
        with self.assertRaisesRegex(ValueError,'link map missing'):I.finish(r,self.map,require_map=True)

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
    ap.add_argument('--mutant',choices=['compile-as-driver','internal-definitions','positional-inputs'])
    args,rest=ap.parse_known_args();OUT=args.out.resolve();OUT.mkdir(parents=True,exist_ok=False)
    MUTANT=args.mutant;I=load(MUTANT)
    if MUTANT:
        cases=['Identity.test_cxx_driver_compiles_identity_as_c','Identity.test_generated_source_is_external_as_cxx']
        if MUTANT=='positional-inputs':
            cases=['Identity.test_library_change_changes_binding','Identity.test_linker_script_inputs_are_bound',
                   'Identity.test_driver_default_inputs_are_bound']
        result=unittest.main(argv=[__file__,*cases],exit=False).result
        killed=bool(result.failures or result.errors)
        print(MUTANT,'KILLED' if killed else 'SURVIVED',file=sys.stderr)
        sys.exit(0 if killed else 1)
    unittest.main(argv=[__file__,*rest])
