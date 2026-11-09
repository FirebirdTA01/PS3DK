#!/usr/bin/env python3
"""Real CMake target links for all six wait variants, with a private overlay.

No target execution. A fresh full SDK prefix and Windows consumers are separate
gates; this test reads the provided SDK and never installs into it.
"""
import argparse,json,os,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ['out','toolchain','sdk','runtime-build','profiler-build']:p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--cmake-dir',type=Path,help='Installed CMake modules for a fresh-prefix consumer')
p.add_argument('--installed-prefix',type=Path,help='Private installed variant prefix; refusal controls restore its files')
a=p.parse_args();root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
prefix=(a.installed_prefix or out/'overlay').resolve();commands=[]
if not a.installed_prefix:
    headerdir=prefix/'ppu/include/ps3dk';headerdir.mkdir(parents=True)
    for name in ['prof_wait.h','prof_wait_internal.h']:
        shutil.copyfile(root/'sdk/include/ps3dk'/name,headerdir/name)
    for abi,sub in [('ilp32','ppu/lib'),('lp64','ppu/lib/lp64')]:
        d=prefix/sub;d.mkdir(parents=True);(d/'prof-waits').mkdir()
        shutil.copyfile(a.runtime_build/abi/'librt_profwait.a',d/'librt_profwait.a')
        shutil.copyfile(a.runtime_build/'manifest.json',d/'prof-waits/runtime-manifest.json')
        (d/'prof-waits/librt.a').write_text('INPUT(-lrt_profwait)\n')
        for mode in ['manual','auto','ext']:
            for f in (a.profiler_build/abi/mode).glob('*.a*'):
                if f.name.endswith(('.a','.a.json')):shutil.copyfile(f,d/f.name)
src=out/'consumer';src.mkdir();(src/'nested').mkdir()
(src/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.21)
project(wait_consumer C CXX)
add_subdirectory(nested)
''')
(src/'nested/CMakeLists.txt').write_text('''add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE net netctl_stub gcm_cmd gcm_sys lv2 m)
if(PS3_PROF_WAITS STREQUAL "EXT")
  target_compile_definitions(consumer PRIVATE PS3DK_PROF=1 TEST_LIVE=1)
endif()
if(TEST_FOREIGN)
  target_compile_definitions(consumer PRIVATE TEST_FOREIGN=1)
endif()
if(TEST_WRONG_ABI)
  target_compile_options(consumer PRIVATE -mlp64)
endif()
add_custom_command(TARGET consumer POST_BUILD COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/post-link-ran")
''')
(src/'nested/main.c').write_text('''#include <pthread.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_wait.h>
#ifdef TEST_FOREIGN
int ps3dk_prof_foreign_probe(void) { return 0; }
#endif
int main(int argc, char **argv) {
    pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c=PTHREAD_COND_INITIALIZER;
    (void)argv;
    if(argc==37) { pthread_mutex_lock(&m); pthread_cond_wait(&c,&m); pthread_mutex_unlock(&m); }
    if(argc==36) { ps3dk_prof_wait_config cfg={sizeof(cfg),1048576,64,128}; return ps3dk_prof_wait_configure(&cfg); }
#ifdef TEST_LIVE
    if(argc==38) return ps3dk_prof_live_start(0);
#endif
#ifdef TEST_FOREIGN
    if(argc==39) return ps3dk_prof_foreign_probe();
#endif
    return 0;
}
''')
env=dict(os.environ,PS3DEV=str(a.toolchain.resolve()),PS3DK=str(a.sdk.resolve()),PS3DK_NO_DEFAULT_INCLUDE='1')
def run(name,argv):
    r=subprocess.run(list(map(str,argv)),env=env,capture_output=True,timeout=180)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,argv)),rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    return r
for abi in ['ilp32','lp64']:
    tc=(a.cmake_dir or root/'cmake')/('ps3-ppu-toolchain.cmake' if abi=='ilp32' else 'ps3-ppu-toolchain-lp64.cmake')
    for mode in ['manual','auto','ext']:
        name=abi+'-'+mode;b=out/name
        # A compiler-only retained prefix need not contain CRT/startfiles. Point
        # explicitly at the read-only SDK, without copying into the toolchain.
        base_lib=a.sdk/'ppu/lib'/('lp64' if abi=='lp64' else '')
        startfiles=a.sdk/'ppu/powerpc64-ps3-elf/lib'
        flags='-mcpu=cell -B'+base_lib.as_posix()+'/ -B'+startfiles.as_posix()+'/' + (' -mlp64' if abi=='lp64' else '')
        r=run(name+'-configure',['cmake','-S',src,'-B',b,'-G','Ninja','-DCMAKE_TOOLCHAIN_FILE='+str(tc),
              '-DPS3_PROF_WAITS='+mode.upper(),'-DPS3_PROF_WAITS_ROOT='+str(prefix),'-DCMAKE_BUILD_TYPE=Release',
              '-DCMAKE_C_FLAGS='+flags,'-DCMAKE_CXX_FLAGS='+flags])
        assert r.returncode==0,(name,r.stdout.decode(),r.stderr.decode())
        r=run(name+'-link',['cmake','--build',b,'-j2','--verbose'])
        assert r.returncode==0,(name,r.stdout.decode(),r.stderr.decode())
        assert b'observed archive identity and link selection PASS' in r.stdout,name
        assert (b/'post-link-ran').is_file(),name
        if mode=='ext':
            maptext=(b/'prof-waits/consumer.map').read_text()
            assert 'ps3dk_prof_live_start' in maptext and 'live_api.o' in maptext,name
        print(name,'link and guard PASS',flush=True)

# Real compiler/linker gates, with a post-link sentinel standing in for SELF
# packaging. Each refusal must leave that sentinel absent.
negative=[]
for abi in ['ilp32','lp64']:
    b=out/(abi+'-manual');marker=b/'post-link-ran'
    marker.unlink()
    r=run(abi+'-foreign-configure',['cmake','-S',src,'-B',b,'-DTEST_FOREIGN=ON'])
    assert r.returncode==0,r.stderr.decode()
    r=run(abi+'-foreign-link',['cmake','--build',b,'-j2'])
    assert r.returncode!=0 and b'foreign profiler provider: ps3dk_prof_foreign_probe' in r.stdout,(r.stdout,r.stderr)
    assert not marker.exists()
    negative.append(abi+'-foreign-provider')
    r=run(abi+'-restore',['cmake','-S',src,'-B',b,'-DTEST_FOREIGN=OFF'])
    assert r.returncode==0
    lib=prefix/'ppu/lib'/('lp64' if abi=='lp64' else '')
    archive=lib/'libps3dkprof_wait.a'
    saved=archive.read_bytes()
    try:
        archive.unlink()
        r=run(abi+'-missing-link',['cmake','--build',b,'-j2'])
        assert r.returncode!=0 and not marker.exists()
        negative.append(abi+'-missing-archive')
        archive.write_bytes(saved+b'changed')
        r=run(abi+'-changed-link',['cmake','--build',b,'-j2'])
        assert r.returncode!=0 and b'archive hash mismatch' in r.stdout,(r.stdout,r.stderr)
        assert not marker.exists()
        negative.append(abi+'-changed-archive')
    finally:
        archive.write_bytes(saved)
    # Change only our private redirect. This exercises a real GNU map with the
    # default runtime/core admitted first, rather than a fabricated map string.
    shim=lib/'prof-waits/librt.a';original_shim=shim.read_bytes()
    default_lib=a.sdk/'ppu/lib'/('lp64' if abi=='lp64' else '')
    try:
        for tag,script,reason in [
            ('default-runtime','INPUT("'+(default_lib/'librt.a').as_posix()+'")\n','default librt member selected'),
            ('mixed-core','EXTERN(ps3dk_prof_init)\nINPUT("'+(default_lib/'libps3dkprof.a').as_posix()+'" -lrt_profwait)\n','mixed profiler archive')]:
            shim.write_text(script)
            (src/'nested/main.c').touch()
            r=run(abi+'-'+tag,['cmake','--build',b,'-j2'])
            assert r.returncode!=0 and reason.encode() in r.stdout,(tag,r.stdout,r.stderr)
            assert not marker.exists()
            negative.append(abi+'-'+tag)
    finally:
        shim.write_bytes(original_shim)
    if abi=='ilp32':
        r=run('wrong-abi-configure',['cmake','-S',src,'-B',b,'-DTEST_WRONG_ABI=ON'])
        assert r.returncode==0
        r=run('wrong-abi-build',['cmake','--build',b,'-j2'])
        assert r.returncode!=0 and b'observed wait archive and target data models differ' in r.stdout,(r.stdout,r.stderr)
        assert not marker.exists()
        negative.append('target-data-model-drift')
        r=run('restore-abi',['cmake','-S',src,'-B',b,'-DTEST_WRONG_ABI=OFF'])
        assert r.returncode==0
    r=run(abi+'-restored-link',['cmake','--build',b,'-j2'])
    assert r.returncode==0 and marker.is_file(),(r.stdout,r.stderr)
print('real refusal controls',len(negative),'PASS',flush=True)
(out/'RESULT.json').write_text(json.dumps(dict(links=6,negative_controls=negative,restored_links=2,failures=0,scope=__doc__),indent=2)+'\n')
