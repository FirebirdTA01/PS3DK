#!/usr/bin/env python3
"""Compile the complete pthread TU; compare OFF against a pinned baseline.

Compile/disassembly evidence only. Does not link an observed archive, execute
on target, prove callbacks' ABI behavior, or measure whole-program stack.
"""
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True);p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--sdk-include',type=Path,required=True);p.add_argument('--baseline',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
cc=a.compiler.resolve();prefix=str(cc).removesuffix('gcc');commands=[];dependencies={}
def run(name,argv):
    r=subprocess.run(list(map(str,argv)),capture_output=True,timeout=120)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,argv)),rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2))
    assert r.returncode==0,(name,r.stderr.decode());return r.stdout
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
run('compiler',[cc,'-v'])
candidate=root/'runtime/lv2/librt/pthread.c';header=candidate.with_name('prof_wait_adapter.h')
shutil.copyfile(header,out/header.name);shutil.copyfile(candidate,out/'candidate.c');shutil.copyfile(a.baseline,out/'baseline.c')
source=out/'pthread.c';results=[]
for abi,af in [('ilp32',[]),('lp64',['-mlp64'])]:
    for opt in ['-O0','-O2']:
        tag=abi+'-'+opt[1:]
        flags=[*af,opt,'-g0','-mcpu=cell','-fexceptions','-Werror=implicit-function-declaration','-Werror=incompatible-pointer-types','-Werror=int-conversion','-Wno-pointer-to-int-cast','-I',out,'-I',root/'sdk/include','-I',a.sdk_include]
        for name,src,extra in [('base',a.baseline,[]),('off',candidate,[]),('on',candidate,['-DPS3DK_PROF_WAIT_OBSERVE=1'])]:
            source.write_bytes(src.read_bytes());stem=tag+'-'+name
            run(stem,[cc,*flags,*extra,'-fstack-usage','-MD','-MF',out/(stem+'.d'),'-c',source,'-o',out/(stem+'.o')])
            run(stem+'-cpp',[cc,*flags,*extra,'-E','-P',source])
            run(stem+'-symbols',[prefix+'nm','-a',out/(stem+'.o')])
            if name=='on':run(stem+'-disassembly',[prefix+'objdump','-dr',out/(stem+'.o')])
            dep=(out/(stem+'.d')).read_text().replace('\\\n',' ').split(':',1)[1].split()
            for d in dep:
                path=Path(d)
                if path!=source:dependencies[str(path)]=sha(path)
        assert (out/(tag+'-base.o')).read_bytes()==(out/(tag+'-off.o')).read_bytes(),tag+' OFF object differs'
        def tokens(name):return re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\w+|\S',(out/(tag+'-'+name+'-cpp.stdout')).read_text())
        assert tokens('base')==tokens('off'),tag+' OFF tokens differ'
        off=(out/(tag+'-off-symbols.stdout')).read_text();on=(out/(tag+'-on-symbols.stdout')).read_text()
        assert 'ps3dk_prof__wait_' not in off and '__ps3dk_prof_wait_' not in off
        for s in ['ps3dk_prof__wait_begin','ps3dk_prof__wait_end','__ps3dk_prof_wait_thread_start','__ps3dk_prof_wait_thread_exit']:
            assert re.search(r'\bw '+s+r'$',on,re.M),(tag,s)
        assert 'pt_wait_observer_busy' in on and 'pt_wait_observer_busy' not in off
        results.append(dict(abi=abi,opt=opt,off_object_equal=True,off_tokens_equal=True,observed_weak_hooks=4,baseline_object_sha256=sha(out/(tag+'-base.o')),observed_object_sha256=sha(out/(tag+'-on.o'))))
        print(tag,'OFF identity and observed symbols PASS',flush=True)
assert all(sha(Path(n))==h for n,h in dependencies.items()),'header changed during build'
(out/'dependency-pins.json').write_text(json.dumps(dependencies,indent=2))
(out/'RESULT.json').write_text(json.dumps(dict(compiler_sha256=sha(cc),baseline_sha256=sha(a.baseline),candidate_sha256=sha(candidate),header_sha256=sha(header),results=results),indent=2))
