#!/usr/bin/env python3
"""Compare linked OFF/ON storage for the controlled six target consumers.

Builds private baseline archives; never installs or executes target code. TLS
payload and aligned ELF template deltas are distinct, layout-specific results.
"""
import argparse, hashlib, json, os, re, subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
for name in ['out','toolchain','sdk','runtime-baseline','observed-links']:
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[3]
out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
cc=a.toolchain/'ppu/bin/powerpc64-ps3-elf-gcc';tool=str(cc).removesuffix('gcc')
env=dict(os.environ,PS3DEV=str(a.toolchain),PS3DK=str(a.sdk),PS3DK_NO_DEFAULT_INCLUDE='1')
commands=[]
def run(name,argv):
    r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=300)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,argv)),rc=r.returncode))
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    assert r.returncode==0,(name,r.stdout.decode(),r.stderr.decode())
    return r.stdout.decode()

run('baseline-profiler',['make','-C',root/'sdk/libps3dkprof','-j2','shipped',
    'ext-ilp32-profile','ext-lp64-profile','ext-live-ilp32','ext-live-lp64',
    'CROSS='+tool,'PS3DEV='+str(a.toolchain),'PS3DK='+str(a.sdk),
    'PS3_BUILD_ROOT='+str(out/'build'),'SDK_VERSION=v0.27.0+wait-audit'])
src=out/'consumer';src.mkdir()
consumer=(a.observed_links/'consumer/nested/main.c').read_text()
# The observed consumer also checks the opt-in configuration API. That API has
# no OFF provider; remove only its declaration and unreachable probe branch.
optional_header='#include <ps3dk/prof_wait.h>\n'
optional_probe='    if(argc==36) { ps3dk_prof_wait_config cfg={sizeof(cfg),1048576,64,128}; return ps3dk_prof_wait_configure(&cfg); }\n'
if optional_header in consumer:
    assert consumer.count(optional_header)==1 and consumer.count(optional_probe)==1
    consumer=consumer.replace(optional_header,'').replace(optional_probe,'')
(src/'main.c').write_text(consumer)
(src/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.21)
project(wait_baseline C CXX)
link_directories(BEFORE "${BASE_RT}")
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE ${BASE_PROF} net netctl_stub gcm_cmd gcm_sys lv2 m)
target_link_options(consumer PRIVATE "-Wl,-u,ps3dk_prof_init" "-Wl,-Map,${CMAKE_BINARY_DIR}/consumer.map,--cref")
if(BASE_MODE STREQUAL "auto")
  target_link_options(consumer PRIVATE "-Wl,-u,__ps3dk_prof_startup_anchor")
endif()
if(BASE_MODE STREQUAL "ext")
  target_compile_definitions(consumer PRIVATE PS3DK_PROF=1 TEST_LIVE=1)
endif()
''')

def storage(name,elf):
    ph=run(name+'-segments',[tool+'readelf','-lW',elf])
    sy=run(name+'-symbols',[tool+'readelf','-sW',elf])
    tls=re.search(r'^\s*TLS\s+\S+\s+\S+\s+\S+\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)\s*$',ph,re.M)
    assert tls,name+' missing TLS segment'
    symbols={}
    for match in re.finditer(r'^\s*\d+:\s+([0-9a-f]+)\s+(0x[0-9a-f]+|\d+)\s+(TLS|OBJECT)\s+\S+\s+\S+\s+\S+\s+(\S+)\s*$',sy,re.M):
        address,size,kind,symbol=match.groups()
        if kind=='TLS' or symbol in ['g_storage','ps3dk_prof__wait_options']:
            assert symbol not in symbols,(name,'duplicate storage symbol',symbol)
            symbols[symbol]=dict(address=int(address,16),size=int(size,0) if size.startswith('0x') else int(size),kind=kind)
    memory=int(tls[2],16);align=int(tls[3],16)
    return dict(sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),tls_file_bytes=int(tls[1],16),
        tls_memory_bytes=memory,tls_alignment=align,tls_aligned_bytes=(memory+align-1)//align*align,symbols=symbols)

results={}
for abi,af in [('ilp32',[]),('lp64',['-mlp64'])]:
    d=out/('rt-'+abi);d.mkdir()
    objects=sorted((a.runtime_baseline/(abi+'-baseline')).glob('*.o'))
    assert any(f.name=='pthread.o' for f in objects)
    run(abi+'-baseline-runtime',[tool+'ar','rcsD',d/'librt.a',*objects])
    for mode in ['manual','auto','ext']:
        name=abi+'-'+mode;b=out/name
        profroot=out/'build/sdk/libps3dkprof'
        if mode=='ext':
            archives=[profroot/(abi+'-ext-live')/'libps3dkprof_ext_live.a',profroot/(abi+'-ext-profile')/'libps3dkprof_ext_profile.a']
        else:
            archives=[profroot/abi/('libps3prof.a' if mode=='auto' else 'libps3dkprof.a')]
        assert all(f.is_file() for f in archives),archives
        base_lib=a.sdk/'ppu/lib'/('lp64' if abi=='lp64' else '')
        flags='-mcpu=cell -B'+str(base_lib)+'/ -B'+str(a.sdk/'ppu/powerpc64-ps3-elf/lib')+'/' + (' -mlp64' if af else '')
        tc=root/'cmake'/('ps3-ppu-toolchain-lp64.cmake' if af else 'ps3-ppu-toolchain.cmake')
        run(name+'-configure',['cmake','-S',src,'-B',b,'-G','Ninja','-DCMAKE_TOOLCHAIN_FILE='+str(tc),
            '-DPS3_PROF_WAITS=OFF','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_FLAGS='+flags,'-DCMAKE_CXX_FLAGS='+flags,
            '-DBASE_RT='+str(d),'-DBASE_PROF='+';'.join(map(str,archives)),'-DBASE_MODE='+mode])
        run(name+'-link',['cmake','--build',b,'-j2'])
        off=storage(name+'-off',b/'consumer')
        on=storage(name+'-on',a.observed_links/name/'nested/consumer')
        maptext=(b/'consumer.map').read_text()
        assert str(d/'librt.a')+'(pthread.o)' in maptext,name
        assert 'ps3dk_prof__wait_begin' not in maptext,name+' baseline unexpectedly observed'
        ot=off['symbols'];nt=on['symbols']
        assert 'pt_wait_observer_busy' not in ot and nt['pt_wait_observer_busy']['size']==4
        assert ot['ps3dk_prof__g_tls']['size']==nt['ps3dk_prof__g_tls']['size']
        core_delta=nt['g_storage']['size']-ot['g_storage']['size']
        assert core_delta in (0,8),(name,core_delta)
        assert nt['ps3dk_prof__wait_options']['size']==16
        row=dict(off=off,on=on,tls_symbol_payload_delta=sum(v['size'] for v in nt.values() if v['kind']=='TLS')-sum(v['size'] for v in ot.values() if v['kind']=='TLS'),
                 tls_aligned_template_delta=on['tls_aligned_bytes']-off['tls_aligned_bytes'],static_payload_delta=core_delta+16)
        assert row['tls_symbol_payload_delta']==4,row
        results[name]=row
        print(name,'TLS payload',row['tls_symbol_payload_delta'],'aligned template',row['tls_aligned_template_delta'],'static payload',row['static_payload_delta'],flush=True)
(out/'RESULT.json').write_text(json.dumps(dict(scope=__doc__,consumers=results),indent=2)+'\n')
