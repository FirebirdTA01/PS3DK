#!/usr/bin/env python3
"""Real core/PAL/startup capture control with a profiler-free application.

Host-only initial lifecycle gate. No GCM initialization occurs in this consumer;
its required result is NO_GCM_INIT_OBSERVED, not successful RSX attachment.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--case',choices=['noinit','lease','init','race','ops','conflict','reset','stop','stopvisit','frames','clocks','clockcap','segments','flips','present','spanning','fifofail','fifopre','fifopost'],default='noinit')
ap.add_argument('--mutant',choices=['reject-late-end','ignore-global-stop','no-frame-boundary','clock-put',
                                  'fifo-publish','fifo-cancel','fifo-owner','fifo-freeze','retired-owner',
                                  'segment-silent','no-flip-frames','one-record-chunks','no-present-wait',
                                  'poll-wait','poll-scope-cadence','boundary-cadence','boundary-scope'])
a=ap.parse_args()
operations=a.case in ('ops','conflict','reset','frames')
if a.mutant:assert a.case=={'reject-late-end':'stop','ignore-global-stop':'stopvisit','no-frame-boundary':'frames','clock-put':'clocks',
                          'fifo-publish':'fifopost','fifo-cancel':'fifopre','fifo-owner':'init',
                          'fifo-freeze':'init','retired-owner':'init','segment-silent':'segments',
                          'no-flip-frames':'flips','one-record-chunks':'segments','no-present-wait':'flips',
                          'poll-wait':'present','poll-scope-cadence':'present',
                          'boundary-cadence':'spanning','boundary-scope':'spanning'}[a.mutant] and not a.sanitize
root=Path(__file__).resolve().parents[3]
src=root/'sdk/libps3dkprof/src'
assert (src/'auto_rsx/auto_runtime.c').is_file(), 'AUTO_RUNTIME_IMPLEMENTATION'
if a.case=='lease':
    assert (src/'auto_rsx/auto_lease.h').is_file(), 'AUTO_SHARED_LEASE_IMPLEMENTATION'
if a.case=='init':
    assert 'int ps3dk_prof_auto_attach(' in (src/'auto_rsx/auto_runtime.c').read_text(), 'AUTO_ATTACH_IMPLEMENTATION'
if operations:
    assert 'uint32_t ps3dk_prof__auto_begin(' in (src/'auto_rsx/auto_runtime.c').read_text(), 'AUTO_MARKER_PRODUCER'
    assert 'void ps3dk_prof__auto_resource_write(' in (src/'auto_rsx/auto_runtime.c').read_text(), 'AUTO_RESOURCE_INVALIDATION'
out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
inc=out/'include/ps3dk';inc.mkdir(parents=True)
for name in ('prof.h','prof_fifo.h','prof_auto.h'):
    shutil.copyfile(root/'sdk/include/ps3dk'/name,inc/name)
for name,rel in [('prof_gpu.h','rsx/prof_gpu.h'),('prof_spu.h','spu/prof_spu.h'),('psp_desc.h','spu/psp_desc.h')]:
    shutil.copyfile(src/rel,inc/name)
commands=[]
def run(name,cmd,cwd=out):
    cmd=list(map(str,cmd))
    r=subprocess.run(cmd,capture_output=True,cwd=cwd,timeout=90,
                     env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=cmd,rc=r.returncode,cwd=str(cwd)))
    (out/'commands.json').write_bytes((json.dumps(commands,indent=2)+'\n').encode())
    if name=='run' and a.mutant and a.mutant not in ('clock-put','retired-owner','one-record-chunks'):
        expected={'reject-late-end':b'!g_tls.in_gpu && g_tls.acked',
                  'ignore-global-stop':b'g_tls.acked && !g_tls.in_gpu',
                  'no-frame-boundary':b'context.current==words+18',
                  'fifo-publish':b'g_fifo.state==FS_SEALED',
                  'fifo-cancel':b'auto_test_fifo_installs==1 && auto_test_fifo_removes==1',
                  'fifo-owner':b'p_gpu_fifo_owner(g_prof,g_fifo.domain,&context)==PS3DK_PROF_EPERM',
                  'fifo-freeze':b'ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_ESTOPPED',
                  'segment-silent':b'ticket && app==before+2',
                  'no-flip-frames':b'counts[0]==i+1',
                  'no-present-wait':b'present[2]==1 && present[3]>0',
                  'poll-wait':b'p[0]==0 && p[1]==0',
                  'poll-scope-cadence':b'p[2]==0 && p[4]==0',
                  'boundary-cadence':b'p[2]==0 && p[3]==0 && p[5]==1',
                  'boundary-scope':b'p[0]==0 && p[1]==0 && p[6]==1'}[a.mutant]
        assert r.returncode!=0 and b'Assertion' in r.stderr and expected in r.stderr,r.stderr.decode()
        print(a.mutant+' rejected by '+expected.decode())
        raise SystemExit(0)
    assert r.returncode==0,(name,r.returncode,r.stderr.decode())
    return r

flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-prio-ctor-dtor','-pthread',
       '-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_HOST','-DPS3DK_PROF_WITH_SPU',
       '-DPS3DK_PROF_WITH_PROFILE','-DPS3DK_PROF_WITH_FIFO','-DPS3DK_PROF_WITH_AUTO',
       '-I'+str(inc.parent),'-I'+str(root/'sdk/libgcm_cmd/src')]
for directory in ('core','pal','spu','rsx','fifo','auto_rsx'):flags+=['-I'+str(src/directory)]
if a.case!='noinit':flags+=['-DPS3DK_PROF_TEST_HOOKS']
if operations or a.case in ('stop','stopvisit'):
    flags+=['-DGPP_HOST_DEVMEM','-DTEST_INVALIDATE='+str({'ops':0,'conflict':1,'reset':2,'stop':0,'stopvisit':0,'frames':0}[a.case])]
if a.case=='frames':flags+=['-DTEST_FRAMES=1']
if a.case in ('clocks','clockcap'):flags+=['-DGPP_HOST_DEVMEM','-DTEST_CLOCK_FRAMES='+('256' if a.case=='clockcap' else '64')]
if a.case=='stopvisit':flags+=['-DTEST_STOP_VISIT=1']
if a.case in ('segments','flips','present','spanning'):flags+=['-DGPP_HOST_DEVMEM']
if a.case=='flips':
    # only the flip entry-point header: the SDK's own libc/pthread headers must not shadow the host ones
    (inc.parent/'cell/gcm').mkdir(parents=True)
    shutil.copyfile(root/'sdk/include/cell/gcm/ps3tc_gcm_flip.h',inc.parent/'cell/gcm/ps3tc_gcm_flip.h')
    flags+=['-DPS3TC_PROF_AUTO=1']
if a.case=='spanning':flags+=['-DTEST_SPANNING=1']
if a.case in ('fifofail','fifopre','fifopost'):flags+=['-DTEST_FIFO_POINT='+str({'fifofail':0,'fifopre':3,'fifopost':4}[a.case])]
if a.sanitize:flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
if a.sanitize=='address':
    # Same host TLS relaxation constraint as arcs-lifecycle-test: the linker
    # rewrites ADD to LEA while the UBSan null check consumes its flags. All TLS
    # is in this executable; local-exec prevents that relaxation. Keep all
    # sanitizer checks enabled, including null checks.
    flags+=['-ftls-model=local-exec','-fno-omit-frame-pointer']
cc=os.environ.get('CC','cc')
sources=['core/prof_core.c','core/prof_drain.c','core/prof_version.c','pal/pal_host.c',
         'spu/psp_ppu.c','spu/psp_spu.c','spu/ext_runtime.c','spu/psp_elf.c','spu/psp_host_emul.c',
         'rsx/gpu_runtime.c','rsx/prg.c','rsx/gpu_port_gcm.c','rsx/rsxcap.c',
         'fifo/fifo_runtime.c','fifo/fifo_writer.c',
         'auto_rsx/auto_runtime.c','auto_rsx/auto_startup.c','auto_rsx/auto_config.c',
         'auto_rsx/auto_wire.c','auto_rsx/auto_writer.c','auto_rsx/auto_queue.c',
         'auto_rsx/auto_commands.c','auto_rsx/auto_completion.c','auto_rsx/auto_frames.c','auto_rsx/auto_clock.c']
objects=[]
for rel in sources:
    source=src/rel;obj=out/(source.stem+'.o')
    if a.mutant in ('fifo-publish','fifo-cancel','fifo-freeze') and rel=='fifo/fifo_runtime.c':
        code=source.read_text()
        if a.mutant=='fifo-publish':
            old='if(CAS_ACQREL(&g_fifo.state,&expected,(uint32_t)FS_ATTACHED))'
            new='if((void)expected, (ST_REL(&g_fifo.state,(uint32_t)FS_ATTACHED),1))'
        elif a.mutant=='fifo-cancel':
            old='(void)ps3tc_fifo_observe_remove((struct _gcmCtxData *)f->ctx,&f->reg);'
            new='(void)f;'
        else:
            old='ps3dk_prof__auto_fifo_boundary(f->ctx);';new='(void)f;'
        assert code.count(old)==1
        source=out/'fifo_runtime.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant=='fifo-owner' and rel=='rsx/gpu_runtime.c':
        code=source.read_text()
        old='''else if(!g_tls.slot1 || g_tls.acked || g_tls.retired ||
                a->owner_incarnation!=g_tls.incarnation || a->owner_generation!=g_tls.generation ||
                !pal_owner_is_self(&a->owner))'''
        assert code.count(old)==1
        source=out/'gpu_runtime.c';source.write_bytes(code.replace(old,'else if(0)').encode())
    elif a.mutant=='retired-owner' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='if(!capture->owner_declared) {';assert code.count(old)==1
        source=out/'auto_runtime.c'
        source.write_bytes(code.replace(old,'if(p->dr.decl_gen[capture->owner_slot]!=capture->owner_generation) {').encode())
    elif a.mutant=='no-frame-boundary' and rel=='core/prof_core.c':
        code=source.read_text();old='p_auto_frame(fid,tb);';assert code.count(old)==1
        source=out/'prof_core.c';source.write_bytes(code.replace(old,'(void)fid;').encode())
    elif a.mutant=='no-flip-frames' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='    a->in_flip_frame=1;\n    ps3dk_prof_frame();\n';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    a->in_flip_frame=1;\n').encode())
    elif a.mutant=='no-present-wait' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='else {add32(&f->cadence_runs);add64(&f->cadence,a->cadence_ticks);}'
        assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'else (void)f;').encode())
    elif a.mutant=='poll-wait' and rel=='auto_rsx/auto_runtime.c':
        # review finding 1 reintroduced: the interval between a pending and a done poll counts as a wait
        code=source.read_text();old='    if(a->cadence_open) cadence_close(a,cadence_gap(a,before));\n}'
        assert code.count(old)==1
        new=('    if(a->cadence_open && a->coverage.active) {add32(&a->coverage.live.flip_waits);'
             'add64(&a->coverage.live.flip_wait,before-a->cadence_last);}\n'+old[4:])
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    '+new).encode())
    elif a.mutant in ('boundary-cadence','boundary-scope') and rel=='auto_rsx/auto_runtime.c':
        # review finding 2 reintroduced: a boundary no longer takes ownership away from an open run or scope
        code=source.read_text()
        old={'boundary-cadence':'    if(a->cadence_open) {\n        a->cadence_spanning=1;',
             'boundary-scope':'    if(a->flip_wait_open) {\n        a->flip_wait_spanning=1;'}[a.mutant]
        assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    if(0) {'+old.split('{',1)[1]).encode())
    elif a.mutant=='poll-scope-cadence' and rel=='auto_rsx/auto_runtime.c':
        # polls inside a flip-wait scope counted again as cadence
        code=source.read_text();old='if(!before || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return;'
        assert code.count(old)==1
        new='if(!before || !(a=owner_capture(NULL,0))) return;'
        code=code.replace(old,new)
        old2='if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return 0;\n    return observation_tb()|1u;'
        assert code.count(old2)==1
        code=code.replace(old2,'if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0))) return 0;\n    return observation_tb()|1u;')
        source=out/'auto_runtime.c';source.write_bytes(code.encode())
    elif a.mutant=='one-record-chunks' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='enum { AUTO_OPS_BATCH=64 };';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'enum { AUTO_OPS_BATCH=1 };').encode())
    elif a.mutant=='segment-silent' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='if(!a) return foreign_begin(ctx,call,caller);';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'if(!a) {(void)foreign_begin;return 0;}').encode())
    elif a.mutant=='clock-put' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='!auto_clock_after(&a->clocks,a->clock_put_frontier)';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'0').encode())
    elif a.mutant in ('reject-late-end','ignore-global-stop') and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text()
        old='if(!ticket) return;' if a.mutant=='reject-late-end' else 'if(LD_ACQ(&g_prof->stop_latch) || !auto_config_can_admit(&configuration))'
        replacement='if(!ticket || LD_ACQ(&g_prof->stop_latch)) return;' if a.mutant=='reject-late-end' else 'if(!auto_config_can_admit(&configuration))'
        assert code.count(old)==1
        code=code.replace(old,replacement)
        source=out/'auto_runtime.c';source.write_bytes(code.encode())
    run('compile-'+source.stem,[cc,*flags,'-c',source,'-o',obj]);objects.append(obj)
run('archive-members',['ar','rcsD',out/'members.a',*objects])
digest=root/'sdk/libps3dkprof/tools/archive_digest.py'
identity=run('identity',['python3',digest,'emit',out/'members.a','--id','AUTO-HOST','--premises','463','--kinds','1'])
(out/'prof_identity.c').write_bytes(identity.stdout)
run('compile-identity',[cc,*flags,'-c',out/'prof_identity.c','-o',out/'prof_identity.o'])
run('archive',['ar','rcsD',out/'libauto.a',*objects,out/'prof_identity.o'])
# Owner-only fake observer registry. It measures installation and removal, not
# target command-buffer admission, device progress or timestamp correctness.
(out/'bridge.c').write_text('''#include "ps3tc_fifo_observe.h"
unsigned auto_test_fifo_installs,auto_test_fifo_removes;
int auto_test_fifo_fail;
static struct _gcmCtxData *context;
const ps3tc_fifo_observer *auto_test_fifo_observer;
int ps3tc_fifo_observe_install(struct _gcmCtxData *c,const ps3tc_fifo_observer *o,uint32_t *b,uint32_t *e) {
 if(auto_test_fifo_fail || context || !c || !o)return -1;
 context=c;auto_test_fifo_observer=o;++auto_test_fifo_installs;*b=4096;*e=8192;return 0;
}
int ps3tc_fifo_observe_remove(struct _gcmCtxData *c,const ps3tc_fifo_observer *o) {
 if(c!=context || o!=auto_test_fifo_observer)return -1;
 context=0;auto_test_fifo_observer=0;++auto_test_fifo_removes;return 0;
}
const unsigned char ps3dk_prof__auto_mechanism[32]={42};
const unsigned char ps3dk_prof__auto_build_binding[32]={99};
const char ps3dk_prof__auto_build_id[]="host-no-gcm-init";
const char ps3dk_prof__auto_app[]="auto-noinit";
/* Firmware flip calls (libgcm_sys NID stub names) for the flips case, which reaches them through the
 * cellGcm entry points of <cell/gcm/ps3tc_gcm_flip.h> under both spellings. */
int auto_test_fw_flip_rc,auto_test_fw_flips,auto_test_fw_prepares,auto_test_fw_polls,auto_test_fw_pending;
int test_fw_flip(void *c,unsigned char i) __asm__("cellGcmSetFlip");
int test_fw_flip(void *c,unsigned char i) {(void)c;(void)i;++auto_test_fw_flips;return auto_test_fw_flip_rc;}
unsigned test_fw_prepare(void *c,unsigned char i) __asm__("cellGcmSetPrepareFlip");
unsigned test_fw_prepare(void *c,unsigned char i) {(void)c;(void)i;++auto_test_fw_prepares;return auto_test_fw_flip_rc ? 0xffffffffu:3u;}
unsigned test_fw_status(void) __asm__("cellGcmGetFlipStatus");
unsigned test_fw_status(void) {++auto_test_fw_polls;if(auto_test_fw_pending>0) {--auto_test_fw_pending;return 1;}return 0;}
''',encoding='utf-8')
application='#include <stdio.h>\nint main(void) { puts("application complete"); return 0; }\n'
assert 'prof' not in application
if a.case=='lease':application=Path(__file__).with_name('auto-lease-test.c').read_text()
if a.case=='init':application=Path(__file__).with_name('auto-attach-test.c').read_text()
if a.case=='race':application=Path(__file__).with_name('auto-attach-race-test.c').read_text()
if a.case in ('fifofail','fifopre','fifopost'):application=Path(__file__).with_name('auto-fifo-test.c').read_text()
if operations:application=Path(__file__).with_name('auto-operations-test.c').read_text()
if a.case in ('clocks','clockcap'):application=Path(__file__).with_name('auto-clock-runtime-test.c').read_text()
if a.case in ('stop','stopvisit'):application=Path(__file__).with_name('auto-operations-stop-test.c').read_text()
if a.case=='segments':application=Path(__file__).with_name('auto-segment-runtime-test.c').read_text()
if a.case=='flips':application=Path(__file__).with_name('auto-flip-runtime-test.c').read_text()
if a.case in ('present','spanning'):application=Path(__file__).with_name('auto-present-runtime-test.c').read_text()
(out/'main.c').write_text(application,encoding='utf-8')
run('link',[cc,*flags,out/'main.c',out/'bridge.c','-Wl,-u,__ps3dk_prof_auto_startup_anchor',out/'libauto.a','-o',out/'app'])
capture_dir=out/'capture';capture_dir.mkdir()
cmd=[out/'app']
if a.sanitize=='thread':cmd=['setarch','x86_64','-R',*cmd]
result=run('run',cmd,capture_dir)
if a.case=='lease':
    assert result.stdout==b'AUTO/manual shared reservation PASS\n',result.stdout
    print(result.stdout.decode(),end='')
    raise SystemExit(0)
assert result.stdout==(b'AUTO initial attach and override without device writes PASS\n' if a.case=='init'
                       else b'AUTO actual marker producer and cumulative completion PASS\n' if operations
                       else b'AUTO in-flight owner seal retains claim and lower bound PASS\n' if a.case=='stop'
                       else b'AUTO stopped owner visit acknowledges without marker PASS\n' if a.case=='stopvisit'
                       else b'AUTO actual bounded scheduled clock capture PASS\n' if a.case in ('clocks','clockcap')
                       else b'AUTO segment timing and frame marks PASS\n' if a.case=='segments'
                       else b'AUTO flip-driven frames PASS\n' if a.case=='flips'
                       else b'AUTO presentation evidence PASS\n' if a.case in ('present','spanning')
                       else b'AUTO attach/seal blocked owner refused without publication PASS\n' if a.case=='race'
                       else b'AUTO FIFO refusal and unpublished cleanup PASS\n' if a.case in ('fifofail','fifopre','fifopost')
                       else b'application complete\n'),result.stdout
files=list(capture_dir.glob('*.prof'));assert len(files)==1,files
data=files[0].read_bytes()
assert data[:8]==b'PS3DKPRF'
assert struct.unpack_from('<HHI',data,8)==(1,0,195)
pos=16;sequence=0;chunks=[]
while pos<len(data):
    assert len(data)-pos>=20
    kind,n,seq=struct.unpack_from('<IIQ',data,pos)
    assert seq==sequence and n<=1048576 and n<=len(data)-pos-20
    crc=struct.unpack_from('<I',data,pos+16+n)[0]
    assert crc==zlib.crc32(data[pos:pos+16+n])&0xffffffff
    chunks.append((kind,data[pos+16:pos+16+n]));sequence+=1;pos+=20+n
if a.mutant=='retired-owner':
    assert any(k==7 for k,b in chunks),'the owner really retired'
    assert not any(k in (6,37) for k,b in chunks),'revoked declaration prevents terminal capture'
    print('retired-owner rejected: accepted owner declaration revoked at retirement, no FINAL/trailer')
    raise SystemExit(0)
if a.case in ('race','fifopre'):
    assert not any(k==6 or 34<=k<=38 for k,b in chunks),'no trailer or partial AUTO state'
    assert not any(31<=k<=33 for k,b in chunks),'no unpublished FIFO state'
    print('real capture attach/seal race: incomplete file, no resurrected reservation PASS')
    raise SystemExit(0)
auto=[(k,b) for k,b in chunks if 34<=k<=38]
if a.case=='fifopost':
    assert [k for k,b in auto]==[34,37]
    assert struct.unpack_from('<HHI',auto[-1][1])==(1,11,1),'unacknowledged AUTO owner'
    assert not any(31<=k<=33 for k,b in chunks),'cancelled FIFO never published'
    assert chunks[-1][0]==6
    assert struct.unpack_from('<I',chunks[-1][1])[0]&1,'owner did not acknowledge shutdown'
    print('AUTO accepted before seal, unpublished FIFO cancelled, owner remains UNKNOWN PASS')
    raise SystemExit(0)
if a.case=='segments':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(31,11),'CONFIG kind/form masks of the presentation writer'
    records=[]
    for k,b in auto:
        if k!=35:continue
        n=struct.unpack_from('<I',b,8)[0]
        for i in range(n):
            q=b[16+128*i:16+128*(i+1)]
            records.append(dict(id=struct.unpack_from('<Q',q,0)[0],kind=struct.unpack_from('<H',q,12)[0],
                flags=struct.unpack_from('<H',q,14)[0],frame=struct.unpack_from('<Q',q,16)[0],
                raw=struct.unpack_from('<2Q',q,40),args=struct.unpack_from('<7I',q,64),
                segment=struct.unpack_from('<I',q,92)[0],call=struct.unpack_from('<I',q,116)[0],
                serials=struct.unpack_from('<2I',q,104),published=struct.unpack_from('<Q',q,120)[0]))
    shape=[(r['kind'],r['flags'],r['frame'],r['segment'],r['call'],r['serials']) for r in records]
    expected=[(4,3,0,0,0,(1,2)),(3,3,0,0,0,(3,4)),(1,67,0,1,1,(5,6)),(2,67,0,1,1,(5,6)),(2 if False else 3,64|4|16,0,2,0,(7,8)),
              (4,3,0,0,0,(9,10)),(4,3,1,0,0,(11,12)),(1,67,1,1,2,(13,14)),(4,3,1,0,0,(15,16))]
    assert shape==expected,shape
    # every record completes at once when the device writes the label, so one batched OPS chunk carries all nine
    ops_chunks=[b for k,b in auto if k==35]
    if a.mutant=='one-record-chunks':
        assert len(ops_chunks)==9,len(ops_chunks)
        print('one-record-chunks rejected: '+str(len(ops_chunks))+' OPS chunks where the batch is one')
        raise SystemExit(0)
    assert len(ops_chunks)==1,('one batched OPS chunk',len(ops_chunks))
    assert [r['args'][0] for r in records if r['kind']==4]==[1,2,1,2],'FRAME begin/end roles'
    assert all(r['raw'][0]<=r['raw'][1] and r['raw'][0] for r in records if r['flags']&2)
    assert records[4]['raw']==(0,0) and records[4]['published']==16
    coverage=[b for k,b in auto if k==36];assert len(coverage)==2
    for b,(frame,unsupported,rows_expected) in zip(coverage,[(0,1,[(1,1),(1,1),(2,2)]),(1,0,[(1,1),(0,0),(0,0)])]):
        assert len(b)==384 and struct.unpack_from('<H',b,2)[0]==3|64
        assert struct.unpack_from('<Q',b,16)[0]==frame
        assert struct.unpack_from('<Q',b,40)[0]==unsupported,('unsupported_form_visits',frame,b[40:48])
        rows=[struct.unpack_from('<12Q',b,64+96*k) for k in range(3)]
        assert [r[:2] for r in rows]==rows_expected and all(not any(r[2:]) for r in rows),rows
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    assert fields[1]==1|2|64|512,('FINAL, LOWER_BOUND (a held quarantined pair), SEGMENT_QUARANTINE, FRAMES_EXPLICIT',fields[1])
    assert fields[3:6]==(9,9,9) and fields[17]==1,fields # nine records; one quarantined pair held
    print('real capture: segment builds timed per CALL, a never-called build quarantined, FRAME marks PASS')
    raise SystemExit(0)
if a.case=='flips':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(31,11)
    rows=[b for k,b in auto if k==36];assert len(rows)==22,len(rows)
    assert all(len(b)==384 for b in rows)
    flags=[struct.unpack_from('<H',b,2)[0] for b in rows]
    assert flags==[1|2|32|64]*19+[1|2|16|32|64,1|2|16|64,1|2|64],flags # flip rows; flip 19 closed OPEN by the explicit call
    # hand-written polls are polling cadence, never flip-wait scopes: one complete run per flip frame
    scopes=[struct.unpack_from('<I',b,12)+struct.unpack_from('<Q',b,56) for b in rows]
    cadence=[struct.unpack_from('<3I',b,352)+struct.unpack_from('<Q',b,368) for b in rows]
    assert all(x==(0,0) for x in scopes),scopes
    assert all(c[:3]==(1,0,0) and c[3]>0 for c in cadence[:20]) and cadence[20:]==[(0,0,0,0)]*2,cadence
    assert [struct.unpack_from('<Q',b,16)[0] for b in rows]==list(range(22))
    assert all(struct.unpack_from('<12Q',b,64+2*96)[:2]==(1,1) for b in rows),'one observed clear per frame'
    records=[]
    for k,b in auto:
        if k!=35:continue
        for i in range(struct.unpack_from('<I',b,8)[0]):
            q=b[16+128*i:16+128*(i+1)]
            records.append((struct.unpack_from('<H',q,12)[0],struct.unpack_from('<H',q,14)[0],struct.unpack_from('<Q',q,16)[0]))
    assert len(records)==67,len(records)
    assert sum(1 for r in records if r[0]==4)==44 and sum(1 for r in records if r[0]==3)==23
    assert all(r[1]&2 for r in records),'every record raw-valid'
    assert records[0]==(3,2,0),('the clear before the first flip is unassociated',records[0])
    clocks=[b for k,b in auto if k==38];assert len(clocks)==3
    assert [struct.unpack_from('<Q',b,72)[0] for b in clocks]==[0,8,16]
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    assert fields[1]==1|256|512,('FINAL with FRAMES_FLIP and FRAMES_EXPLICIT',fields[1])
    assert fields[4]==67 and fields[5]==67 and fields[10]==1 and fields[17]==0,fields
    print('real capture: frames from flips, explicit precedence, unassociated before the first flip PASS')
    raise SystemExit(0)
if a.case in ('present','spanning'):
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(31,11)
    rows=[b for k,b in auto if k==36]
    n=3 if a.case=='present' else 5
    assert len(rows)==n and all(len(b)==384 for b in rows),[len(b) for b in rows]
    assert [struct.unpack_from('<Q',b,16)[0] for b in rows]==list(range(n))
    MS=1000000
    def present(b):
        scopes,ticks=struct.unpack_from('<I',b,12)[0],struct.unpack_from('<Q',b,56)[0]
        runs,broken,spanning,scope_spanning=struct.unpack_from('<4I',b,352)
        cadence,timeouts,reserved=struct.unpack_from('<QII',b,368)
        assert reserved==0
        return dict(scopes=scopes,ticks=ticks,runs=runs,broken=broken,spanning=spanning,
                    scope_spanning=scope_spanning,cadence=cadence,timeouts=timeouts)
    r=[present(b) for b in rows]
    assert all(struct.unpack_from('<H',b,2)[0]&64 for b in rows)
    if a.case=='present':
        assert (r[0]['scopes'],r[0]['ticks'],r[0]['runs'],r[0]['broken'])==(0,0,0,1),r[0]
        assert r[1]['scopes']==0 and r[1]['runs']==1 and 20*MS<=r[1]['cadence']<21*MS,r[1]
        assert (r[2]['scopes'],r[2]['timeouts'],r[2]['runs'],r[2]['broken'])==(2,1,0,1) and 23*MS<=r[2]['ticks']<25*MS,r[2]
        assert all(x['spanning']==0 and x['scope_spanning']==0 for x in r),r
        fields=struct.unpack_from('<HHI15Q',auto[-1][1])
        assert fields[1]==1|256,fields
        print('real capture: flip-wait scopes counted once, polling cadence and two-poll intervals never a wait PASS')
    else:
        # boundary-spanning runs and scopes: counted in both frames, charged to neither
        assert (r[0]['runs'],r[0]['spanning'],r[0]['scopes'])==(0,1,0),r[0]
        assert (r[1]['runs'],r[1]['spanning'],r[1]['scopes'],r[1]['scope_spanning'])==(0,1,0,1),r[1]
        assert r[2]['scopes']==1 and 2*MS<=r[2]['ticks']<3*MS and r[2]['scope_spanning']==1,r[2]
        assert (r[3]['runs'],r[3]['spanning'])==(0,1) and (r[4]['runs'],r[4]['spanning'])==(0,2),(r[3],r[4])
        fields=struct.unpack_from('<HHI15Q',auto[-1][1])
        assert fields[1]==1|256|512,fields
        print('real capture: runs and scopes across flip-derived and explicit boundaries charged to neither frame PASS')
    raise SystemExit(0)
if a.case in ('clocks','clockcap'):
    frames=256 if a.case=='clockcap' else 64
    schedule=[f for f in range(frames) for condition in (f%8==0,f%64==63) if condition]
    # Sixteen physical slots are reused after validated completion and whole
    # acceptance: the cadence holds past sixteen anchors with no capacity loss.
    rows=[b for k,b in auto if k==38];assert len(rows)==len(schedule),(len(rows),len(schedule))
    if a.mutant=='clock-put':
        assert all(struct.unpack_from('<H',b,2)[0]==8 for b in rows)
        assert all(struct.unpack_from('<2Q',b,48)==(0,0) for b in rows)
        print('clock-put rejected: every real capture clock fact READ_REFUSED, no raw timing accepted')
        raise SystemExit(0)
    owner=struct.unpack_from('<I',rows[0],80)[0];assert owner
    for i,b in enumerate(rows):
        assert len(b)==96 and struct.unpack_from('<HH',b)==(1,1)
        assert struct.unpack_from('<6I',b,4)==(1,1,i+1,0,1024+i%16,i+1)
        assert 1<=struct.unpack_from('<I',b,28)[0]<=100000
        before,after,raw,repeated=struct.unpack_from('<4Q',b,32)
        assert after>=before and raw==repeated==10000*(i+1)
        assert struct.unpack_from('<2I',b,64)==(i+1,i+1)
        assert struct.unpack_from('<QIIQ',b,72)==(schedule[i],owner,0,after-before)
    coverage=[b for k,b in auto if k==36];assert len(coverage)==frames
    assert all(struct.unpack_from('<H',b,2)[0]==3|64 for b in coverage)
    assert all(not any(struct.unpack_from('<36Q',b,64)) for b in coverage)
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    marks=2*frames # a begin and an end FRAME mark per frame, each one serial pair
    assert fields[:3]==(1,1|512,1) and fields[3:]==(marks,marks,marks,0,0,frames,0,0,0,len(schedule),len(rows),
        len(schedule)-len(rows),0,2*marks,0),fields
    kinds=[struct.unpack_from('<HH',b,16+128*i+12) for k,b in auto if k==35 for i in range(struct.unpack_from('<I',b,8)[0])]
    assert len(kinds)==marks and all(k==(4,3) for k in kinds),'every FRAME mark raw-valid'
    assert chunks[-1][0]==6
    print('real capture: '+str(len(rows))+' CLOCK facts, '+str(frames)+' closed frames, '+str(len(schedule)-len(rows))+' lost anchors and FINAL accounting PASS')
    raise SystemExit(0)
if a.case in ('stop','stopvisit'):
    assert [k for k,b in auto]==[34,37]
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    active=int(a.case=='stop')
    assert fields[:3]==(1,11 if active else 1,1),fields
    assert fields[3:]==(active,active,0,0,active,0,0,1,0,0,0,0,0,0,active),fields
    assert chunks[-1][0]==6
    print('real capture: '+a.case+' owner acknowledgement and retained-resource accounting PASS')
    raise SystemExit(0)
if operations:
    assert auto[0][0]==34 and auto[-1][0]==37
    ops=[]
    for k,b in auto:
        if k!=35:continue
        assert struct.unpack_from('<HHI',b)==(1,0,1)
        n=struct.unpack_from('<I',b,8)[0];assert len(b)==16+n*128
        ops += [b[16+i*128:16+(i+1)*128] for i in range(n)]
    marks=[b for b in ops if struct.unpack_from('<H',b,12)[0]==4]
    if a.case=='frames':
        # one begin FRAME mark (frame 0); every later mark found no free report pair
        assert len(marks)==1 and struct.unpack_from('<QHH',marks[0])[0]==1
        assert struct.unpack_from('<HH',marks[0],12)==(4,3) and struct.unpack_from('<QQ',marks[0],40)==(50,60)
        assert struct.unpack_from('<I',marks[0],64)[0]==1
    else:
        assert not marks
    first=1+len(marks);published=6 if a.case=='frames' else 4
    ops=[b for b in ops if struct.unpack_from('<H',b,12)[0]!=4]
    assert len(ops)==2
    for i,b in enumerate(ops):
        assert struct.unpack_from('<Q',b)[0]==first+i
        assert struct.unpack_from('<HH',b,12)==(3,3 if a.case=='frames' else 2 if a.case=='ops' else 20)
        assert struct.unpack_from('<QQ',b,40)==(((100,110) if i==0 else (200,220)) if a.case in ('ops','frames') else (0,0))
        assert struct.unpack_from('<Q',b,120)[0]==published
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    if a.case=='frames':
        rows=[b for k,b in auto if k==36];assert len(rows)==3
        for i,b in enumerate(rows):
            assert len(b)==384 and struct.unpack_from('<HHI',b)==(1,(19 if i==1 else 3)|64,1)
            assert struct.unpack_from('<Q',b,16)[0]==i
            assert struct.unpack_from('<Q',b,24)[0]<=struct.unpack_from('<Q',b,32)[0]
            assert not any(struct.unpack_from('<24Q',b,64))
            expected=(6,2,1,1,0,1,1,0,0,0,0,0) if i==0 else (0,)*12 if i==1 else (1,0,0,0,0,0,1,0,0,0,0,0)
            assert struct.unpack_from('<12Q',b,256)==expected,struct.unpack_from('<12Q',b,256)
        owner=struct.unpack_from('<I',rows[0],8)[0];assert owner
        assert all(struct.unpack_from('<I',b,8)[0]==owner for b in rows+ops+marks)
        assert fields[:3]==(1,1|128|512,1),('FINAL, FRAME_MARK_MISSED (later marks found no pair), FRAMES_EXPLICIT',fields)
        assert fields[3:]==(3,3,3,0,0,3,0,1,0,1,1,0,0,6,0),fields
        clocks=[b for k,b in auto if k==38];assert len(clocks)==1
        assert struct.unpack_from('<HH',clocks[0])==(1,1)
        assert chunks[-1][0]==6
        print('real capture: exact frame zero, exclusions, OPEN predecessor and unassociated post-close PASS')
        raise SystemExit(0)
    invalid=a.case!='ops'
    assert fields[:3]==(1,{'ops':1,'conflict':19,'reset':35}[a.case],1),fields
    assert fields[3:]==(2,2,2,0,0,0,0,5 if invalid else 4,0,0,0,0,int(a.case=='conflict'),4,2 if invalid else 0),fields
    assert chunks[-1][0]==6
    print('real capture: '+a.case+' operation disposition and FINAL PASS')
    raise SystemExit(0)
assert [k for k,b in auto]==[34,37], 'AUTO_NO_INIT_CONFIG_STATUS'
config,status=auto[0][1],auto[1][1]
assert len(config)==160 and len(status)==128
if a.case=='init':
    assert struct.unpack_from('<HHI',config)==(1,0,1)
    assert struct.unpack_from('<IIIIII',config,8)==(1026,1,1,1,31,11)
    assert struct.unpack_from('<IIIIIII',config,32)==(64,32,247,246,8,4,2)
    assert 0<struct.unpack_from('<Q',config,72)[0]<=2097152
    domains=[b for k,b in chunks if k==8];assert len(domains)==1
    assert struct.unpack_from('<II',domains[0])==(1,1026)
    finals=[b for k,b in chunks if k==16];assert len(finals)==1
    assert struct.unpack_from('<II',finals[0])==(1,1026)
    kinds=[k for k,b in chunks]
    assert kinds.index(4)<kinds.index(8)<kinds.index(9)<kinds.index(34)<kinds.index(37)<kinds.index(16)
    fifo=[(k,b) for k,b in chunks if 31<=k<=33]
    assert fifo[0][0]==31 and struct.unpack_from('<III',fifo[0][1],4)==(1,1026,1)
    events=[]
    for k,b in fifo:
        if k==32:
            count=struct.unpack_from('<I',b,8)[0]
            assert len(b)==16+count*96
            events.extend(b[16+i*96:16+(i+1)*96] for i in range(count))
    assert len(events)==6
    for i,b in enumerate(events):
        assert struct.unpack_from('<2Q',b)==(i+1,1)
        assert struct.unpack_from('<IHH',b,32)==(1,i+1,0)
    assert fifo[-1][0]==33 and struct.unpack_from('<HHI',fifo[-1][1])==(1,1,1)
    assert struct.unpack_from('<8Q',fifo[-1][1],8)==(6,6,0,0,6,1,1,0)
    assert 1<=struct.unpack_from('<Q',fifo[-1][1],72)[0]<=6
else:
    assert struct.unpack_from('<HHI',config)==(1,1,1)
    assert struct.unpack_from('<Ii',config,24)==(8 if a.case=='fifofail' else 10,0)
    assert config[8:24]==bytes(16) and config[32:64]==bytes(32) and config[72:96]==bytes(24)
assert config[96:128]==bytes([42])+bytes(31) and config[128:]==bytes([99])+bytes(31)
clocks=[b for k,b in chunks if k==2];assert len(clocks)==1
assert struct.unpack_from('<Q',config,64)[0]==struct.unpack_from('<Q',clocks[0],12)[0]>0
assert struct.unpack_from('<HHI',status)==(1,1,1) and status[8:]==bytes(120)
assert chunks[-1][0]==6, 'complete capture trailer'
print('host attach/config/declaration lifecycle PASS' if a.case=='init' else
      'automatic FIFO refusal: CONFIG reason8 + zero FINAL STATUS PASS' if a.case=='fifofail' else
      'profiler-free host application: automatic CONFIG reason10 + zero FINAL STATUS PASS')
