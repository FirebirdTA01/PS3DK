#!/usr/bin/env python3
"""Retain frame inventory and compute the declared observation-path call graph.

The graph is source-reviewed, not inferred from function maxima. Link-provider
checks and target disassembly review remain required. Firmware/kernel, resolver,
deadline, registration, cleanup, caller and asynchronous stacks are not bounded.
"""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--runtime-build',type=Path,required=True)
p.add_argument('--core-build',type=Path,required=True)
p.add_argument('--variant',choices=['manual','auto','ext'],help='Use actual variant archive member .su files')
p.add_argument('--sdk',type=Path,required=True)
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
cc=a.compiler.resolve();tool=str(cc).removesuffix('gcc');commands=[];pins={}
def pin(f):pins[str(f)]=hashlib.sha256(f.read_bytes()).hexdigest()
def run(name,args,binary=False):
    r=subprocess.run(list(map(str,args)),capture_output=True,timeout=60)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,args)),rc=r.returncode))
    (out/'commands.json').write_text(json.dumps(commands,indent=2))
    assert r.returncode==0,(name,r.stderr.decode())
    return r.stdout if binary else r.stdout.decode()
def su(path):
    pin(path);answer={}
    for line in path.read_text().splitlines():
        head,n,kind=line.split('\t');name=head.rsplit(':',1)[1]
        assert kind=='static',(path,line)
        answer[name]=int(n)
    return answer

# Calls below are sequential alternatives, not cumulative stack use. Adding the
# caller frame to max(callee) retains still-live frames. Counting an optimized
# tail call as an ordinary call is conservative. No recursion exists in this
# declared graph. Weak hooks are bound to these exact wait entry points at link.
graph={
 'pthread_mutex_lock':['pt_wait_call'],
 'pthread_mutex_timedlock':['pt_mutex_timed'],
 'pthread_mutex_clocklock':['pt_mutex_timed'],
 'pt_mutex_timed':['pt_wait_call'],
 'pthread_cond_wait':['pt_cond_wait_common'],
 'pthread_cond_timedwait':['pt_cond_wait_common'],
 'pthread_cond_clockwait':['pt_cond_wait_common'],
 'pt_cond_wait_common':['pt_wait_call'],
 'pt_wait_call':['__errno','ps3dk_prof__wait_begin','ps3dk_prof__wait_end','sysMutexLock','sysCondWait','pt_err'],
 'pt_err':['lv2error'], 'lv2error':[], 'sysMutexLock':[], 'sysCondWait':[],
 '__errno':['__getreent'], '__getreent':['memset','sys_ppu_thread_get_id_stub'],
 'memset':[], 'memcpy':[], 'sys_ppu_thread_get_id_stub':[],
 'ps3dk_prof__wait_begin':['ps3dk_prof__ready','ps3dk_prof__wait_actor','count','memset','ps3dk_prof__le16','ps3dk_prof__le32','ps3dk_prof__le64','publish.isra'],
 'ps3dk_prof__wait_end':['ps3dk_prof__ready','ps3dk_prof__wait_actor','ps3dk_prof__wait_ack','publish.isra'],
 'ps3dk_prof__wait_ack':['ps3dk_prof__wait_actor'],
 'publish.isra':['count','ps3dk_prof__le64','ps3dk_prof__le32','ps3dk_prof__tb','ps3dk_prof__wait_record','memcpy'],
 'ps3dk_prof__le64':['ps3dk_prof__le32'], 'ps3dk_prof__tb':['ps3dk_prof__pal_tb'],
 **{n:[] for n in ['ps3dk_prof__ready','ps3dk_prof__wait_actor','count','ps3dk_prof__le16',
                   'ps3dk_prof__le32','ps3dk_prof__wait_record','ps3dk_prof__pal_tb']}
}
roots=['pthread_mutex_lock','pthread_mutex_timedlock','pthread_mutex_clocklock',
       'pthread_cond_wait','pthread_cond_timedwait','pthread_cond_clockwait']
result={}
for abi,af in [('ilp32',[]),('lp64',['-mlp64'])]:
    frames={};inventory={}
    core_su=[(a.core_build/abi/a.variant/(n+'.su') if a.variant else a.core_build/(abi+'-'+n+'.su'))
             for n in ['wait_runtime','wait_config','prof_core','pal_lv2']]
    for path in [a.runtime_build/abi/'pthread.su',a.runtime_build/abi/'lv2errno.su',*core_su]:
        rows=su(path);inventory[str(path)]=rows;frames.update(rows)
    libc=Path(run(abi+'-libc-path',[cc,*af,'-print-file-name=libc.a']).strip())
    assert libc.is_file();pin(libc)
    libframes={}
    for member,func in [('a-errno.o','__errno'),('a-getreent.o','__getreent'),('a-memcpy.o','memcpy'),('a-memset.o','memset')]:
        obj=out/(abi+'-'+member);obj.write_bytes(run(abi+'-'+member+'-extract',[tool+'ar','p',libc,member],True));pin(obj)
        asm=run(abi+'-'+member+'-disassembly',[tool+'objdump','-dr',obj])
        # These pinned libc objects each implement one function. Reject dynamic
        # SP updates or multiple frame allocations; retain raw instructions.
        adjusts=re.findall(r'\bstdu\s+r1,(-?\d+)\(r1\)',asm)
        assert len(adjusts)<=1 and all(int(x)<0 for x in adjusts),(member,adjusts)
        assert not re.search(r'\b(?:stdux|stwux|stwu|subf)\s+r1,',asm),member
        frame=-int(adjusts[0]) if adjusts else 0
        # Include any leaf red-zone accesses as a conservative frame allowance.
        lows=[-int(x) for x in re.findall(r',(-\d+)\(r1\)',asm)]
        frame=max([frame,*lows])
        libframes[func]=frame;frames[func]=frame
    stub=a.sdk/'ppu/lib'/('lp64' if abi=='lp64' else '')/'liblv2_stub.a'
    pin(stub)
    asm=run(abi+'-lv2-stub-disassembly',[tool+'objdump','-dr',stub])
    found=re.search(r'<__sys_ppu_thread_get_id>:(.*?)(?=\n[0-9a-f]+ <|\Z)',asm,re.S)
    assert found and not re.search(r'\b(?:stdu|stwu|stdux|stwux)\s+r1,',found[1]),'stub frame changed'
    frames['sys_ppu_thread_get_id_stub']=0
    active=set();bounds={};paths={}
    def bound(n):
        assert n not in active,'recursive graph'
        if n in bounds:return bounds[n]
        assert n in frames and n in graph,('missing frame/graph',n)
        active.add(n)
        children=[(bound(c),c) for c in graph[n]]
        best=max(children,default=(0,None));bounds[n]=frames[n]+best[0]
        paths[n]=[n]+(paths[best[1]] if best[1] else [])
        active.remove(n);return bounds[n]
    for n in roots:bound(n)
    result[abi]=dict(bound=max(bounds[n] for n in roots),roots={n:bounds[n] for n in roots},
                      paths={n:paths[n] for n in roots},frames={n:frames[n] for n in graph},
                      libc_frames=libframes,full_sdk_frame_inventory=inventory)
    print(abi,result[abi]['bound'],result[abi]['roots'],flush=True)
(out/'RESULT.json').write_text(json.dumps(dict(scope=__doc__,whole_public_call_stack='UNKNOWN',
    graph=graph,abis=result,input_sha256=pins),indent=2)+'\n')
