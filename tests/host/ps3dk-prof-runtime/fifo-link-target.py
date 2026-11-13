#!/usr/bin/env python3
"""Both-ABI installed FIFO links and refusals. Build only, no target execution.

Copies the supplied observed prefix into a fresh output directory before any
negative control. The supplied SDK and installed prefix are read-only.
"""
import argparse,json,os,shutil,subprocess,sys
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
for n in ['out','sdk','installed-prefix']:ap.add_argument('--'+n,type=Path,required=True)
ap.add_argument('--ninja',default='ninja')
a=ap.parse_args();P=a.out.resolve();P.mkdir(parents=True,exist_ok=False)
B=a.sdk.resolve();W=Path(__file__).resolve().parents[3];N=a.ninja
commands=[];env=dict(os.environ,PS3DK=str(B),PS3DEV=str(B),PSL1GHT=str(B))
env['PATH']=os.pathsep.join(str(B/p) for p in ['bin','ppu/bin','spu/bin'])+os.pathsep+env['PATH']
L=P/'logs';L.mkdir();records=[]
def run(name,args,want=0,needle=None):
    r=subprocess.run(list(map(str,args)),env=env,capture_output=True)
    (L/(name+'.stdout')).write_bytes(r.stdout);(L/(name+'.stderr')).write_bytes(r.stderr)
    records.append(dict(name=name,argv=list(map(str,args)),rc=r.returncode,expected=want))
    (L/'commands.json').write_bytes((json.dumps(records,indent=2)+'\n').encode())
    assert (r.returncode==0)==(want==0),(name,r.returncode,r.stdout[-1500:],r.stderr[-1500:])
    if needle:assert needle in ' '.join((r.stdout+r.stderr).decode(errors='replace').split()),(name,needle)
    print(name,r.returncode,flush=True)
def digest(p):
    import hashlib
    return hashlib.sha256(p.read_bytes()).hexdigest()
root=P/'link prefix';shutil.copytree(a.installed_prefix.resolve(),root,dirs_exist_ok=True)
for n in ['ps3-prof-fifo.cmake','ps3-prof-fifo-check.py']:shutil.copyfile(W/'cmake'/n,root/'cmake'/n)
src=P/'link-source';src.mkdir(exist_ok=True)
(src/'main.c').write_bytes(b'int main(void) { return 0; }\n')
(src/'CMakeLists.txt').write_bytes(('cmake_minimum_required(VERSION 3.21)\nproject(fifo_consumer C)\nadd_executable(consumer main.c)\ntarget_link_libraries(consumer PRIVATE ps3dkprof_ext gcm_cmd gcm_sys io sysutil sysmodule net netctl_stub rt lv2 m)\ninclude("'+root.as_posix()+'/cmake/ps3-prof-fifo.cmake")\nps3_target_prof_fifo(consumer ROOT "'+root.as_posix()+'")\n').encode())
for abi in ['ilp32','lp64']:
    lib=root/('ppu/lib/lp64' if abi=='lp64' else 'ppu/lib');base=B/('ppu/lib/lp64' if abi=='lp64' else 'ppu/lib')
    out=P/('link-'+abi);tf=B/'cmake'/('ps3-ppu-toolchain'+('-lp64' if abi=='lp64' else '')+'.cmake')
    configure=['cmake','-S',src,'-B',out,'-G','Ninja','-DCMAKE_MAKE_PROGRAM='+N,'-DCMAKE_TOOLCHAIN_FILE='+str(tf)]
    build=['cmake','--build',out,'--','-v','-j2']
    manifest=lib/'prof-fifo.json';saved=manifest.read_bytes();data=json.loads(saved)
    run(abi+'-configure',configure);run(abi+'-link',build)
    observed=lib/'libgcm_cmd_prof_fifo.a';original=observed.read_bytes()
    try:
        observed.write_bytes(original+b'bad')
        run(abi+'-hash',configure,1,'hash mismatch')
        observed.write_bytes(original)
        data['abi']='wrong';manifest.write_bytes(json.dumps(data).encode())
        run(abi+'-manifest-abi',configure,1,'ABI mismatch')
        manifest.write_bytes(saved)
        other=root/('ppu/lib' if abi=='lp64' else 'ppu/lib/lp64')/'libgcm_cmd_prof_fifo.a'
        observed.write_bytes(other.read_bytes())
        run(abi+'-physical-abi',[sys.executable,root/'cmake/ps3-prof-fifo-check.py','--root',root,'--abi',abi,'--seal','--readelf',B/('ppu/bin/powerpc64-ps3-elf-readelf'+('.exe' if os.name=='nt' else '')),'--source-commit','99a7343326c90ae51784cbf25c8f44cc2f41e47d'],1,'ABI tags')
        observed.write_bytes((base/'libgcm_cmd.a').read_bytes());data=json.loads(saved);data['archives'][observed.name]=digest(observed);manifest.write_bytes(json.dumps(data).encode())
        run(abi+'-default-configure',configure)
        run(abi+'-default-missing-bridge',build,1,'ps3tc_fifo_observe_')
    finally:
        observed.write_bytes(original);manifest.write_bytes(saved)
    (src/'main.c').write_bytes(b'void ps3tc_fifo_observe_install(void) {}\nint main(void) { return 0; }\n')
    run(abi+'-duplicate-configure',configure)
    run(abi+'-duplicate-provider',build,1,'multiple definition')
    (src/'main.c').write_bytes(b'int main(void) { return 0; }\n')
    run(abi+'-restored-configure',configure);run(abi+'-restored-link',build)
