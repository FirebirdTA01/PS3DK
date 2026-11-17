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

# Report-ring mutants: (case, source, old, new, the assertion that must reject it)
RING_MUTANTS={
    'range-by-sizeof':('init','auto_rsx/auto_runtime.c','if(o->size<AUTO_OPTIONS_V36 || !o->range_pairs)',
                       'if(o->size<sizeof *o || !o->range_pairs)',b'reserves[0]==32 && reserves[1]==32'),
    'ring-unleased':('ring','rsx/gpu_runtime.c','(a->ring_label && label==a->ring_label) ||','0 ||',
                     b'ps3dk_prof_gpu_attach((void *)&third,32,16,245,&manual)==PS3DK_PROF_EINVAL'),
    'ring-label-unwatched':('ring','auto_rsx/auto_runtime.c','(a->ring_port.ready && index==a->ring_port.label)','0',
                            b'a->conflicts==1'),
    'no-label-readback':('ring','auto_rsx/auto_runtime.c',
                         'a->ring_device.read_label(a->ring_device.ctx,a->ring_label)!=0','0',
                         b'!ticket && labels[247]==0 && labels[246]==0 && labels[245]==0x5a5a5a5au'),
    'map-failure-reused':('ring','auto_rsx/auto_runtime.c','            ring_retained+=plan->total;return AUTO_ALLOCATION_FAILED;',
                          '            ring_memory.ea=ea;ring_memory.total=plan->total;ring_memory.bytes=plan->bytes;return AUTO_ALLOCATION_FAILED;',
                          b'ps3dk_prof__auto_test_ring_retained()=='),
    'legacy-whole-pool':('ring','auto_rsx/auto_runtime.c','plan->legacy_pairs)) return AUTO_UNSUPPORTED_CONTEXT;',
                         'plan->legacy_pairs+plan->blocks*plan->pairs)) return AUTO_UNSUPPORTED_CONTEXT;',
                         b'a->operations.capacity==16384 && a->operations.pairs==252'),
    'cached-current':('ringwrap','ps3tc_fifo_wrap.c',
                      '    ps3dk_prof__auto_prewrap(ctx);\n#endif\n    volatile uint32_t *const tail = (volatile uint32_t *)ctx->current;',
                      '    volatile uint32_t *const tail = (volatile uint32_t *)ctx->current;\n    ps3dk_prof__auto_prewrap(ctx);\n#endif',
                      b'visible[0]==M(0,0x1d6c,1)'),
    'no-prewrap':('ringwrap','ps3tc_fifo_wrap.c','    ps3dk_prof__auto_prewrap(ctx);\n#endif\n    volatile','#endif\n    volatile',
                  b'context.current==fifo+4 && context.end==fifo+PHYS && !a->tail.active'),
    'cached-end':('ringwrap','auto_rsx/auto_runtime.c','n=auto_ring_flush_packet((uint32_t *)(void *)cur,(end-cur)/4u,',
                  'n=auto_ring_flush_packet((uint32_t *)(void *)cur,(a->tail.visible-cur)/4u,',b'visible[0]==M(0,0x1d6c,1)'),
    'scan-injected':('ringwrap','auto_rsx/auto_runtime.c',
                     '    a->scan=(uintptr_t)(void *)ctx->current; /* the injected packet is never application state */','',
                     b'ps3dk_prof__auto_test_capture()->shadow.known==0xffu'),
    'no-wrap-scan-reset':('ringwrap','auto_rsx/auto_runtime.c',
                          '    a->scan=(uintptr_t)(void *)ctx->begin; /* the wrap resumes at begin; its JUMP is not application state */','',
                          b'a->scan==(uintptr_t)fifo'),
    'custom-callback':('ringwrap','auto_rsx/auto_runtime.c','(LD_ACQ(&a->ring_flush_request) || !ring_native(ctx))','(LD_ACQ(&a->ring_flush_request))',
                       b'a->ring.halted && a->ring_refusal==AUTO_RING_REFUSED_CALLBACK'),
    'end-arbitrary':('ringwrap','auto_rsx/auto_runtime.c','a->tail.physical==physical && end==a->tail.visible',
                     'a->tail.physical==physical && end<=physical',
                     b'!ps3dk_prof__auto_fifo_end_known(&context,fifo+PHYS,visible-1)'),
}
RING_MUTANTS.update({
    # the shadow is not brought up to date before a flush: the split Mode/Format is lost, the app transfer misfires
    'flush-no-scan':('ringrun','auto_rsx/auto_runtime.c','    ring_scan(a,ctx);\n    ST_REL(&a->ring_flush_request,0u);',
                     '    ST_REL(&a->ring_flush_request,0u);',b"dst>=MAIN_IO && dst-MAIN_IO+len<=main_bytes"),
    # the window id ignored: an END and reopen share one block
    'window-aba':('ringrun','auto_rsx/auto_runtime.c','const uint32_t window=p_window_id(g_prof);','const uint32_t window=1;',
                  b'a->ring.active==g+1u && a->ring_flushes==flushes+1u'),
    # no settle at a frame boundary: the open block spans into the next frame
    'no-frame-settle':('ringrun','auto_rsx/auto_runtime.c',
                       "        ring_settle(a); /* the previous frame's block, before its END mark */\n",'',
                       b'!a->tail.active && !a->ring.active'),
    # the ring ordinal acknowledged before whole sink acceptance
    'ring-ack-early':('ringshort','auto_rsx/auto_runtime.c',
                      '    if(!auto_writer_ops(&writer,batch,n)) return 0;\n    /* whole sink acceptance',
                      '    for(i=0;i<n;i++) if(batch_ring[i]) (void)auto_ring_ack(&a->ring,batch[i].ring_generation,'
                      'batch[i].ring_ordinal,1);\n    if(!auto_writer_ops(&writer,batch,n)) return 0;\n    /* whole sink acceptance',
                      b'RETIRED(a)==retired'),
    # metadata exhaustion latching the rest of the frame (the legacy frame-tail rule)
    'ring-meta-latch':('ringmeta','auto_rsx/auto_runtime.c',
                       "                        a->operations.refusal==AUTO_QUEUE_STOPPED ? 9:11;\n"
                       "        (void)auto_frames_count(&a->coverage,call->kind,column);return 0;",
                       "                        a->operations.refusal==AUTO_QUEUE_STOPPED ? 9:11;\n"
                       "        if(column==4) a->pool_tail_column=4;\n"
                       "        (void)auto_frames_count(&a->coverage,call->kind,column);return 0;",
                       b'a->coverage.live.counts[2][1]==timed+10u'),
    # a full block not flushed after its last pair (it would wait for the next draw)
    'no-full-flush':('ringrun','auto_rsx/auto_runtime.c','    if(a->ring.slots[slot].count==a->ring.pairs) ring_flush(a,ctx);\n    else if',
                     '    if(a->ring.slots[slot].count>a->ring.pairs) ring_flush(a,ctx);\n    else if',b'a->ring.slots[(a->ring.active-1u)%256u].count<126u'),
    # the public end-known query without its registered-end and owner-TLS identity checks
    'end-known-registered':('ringwrap','auto_rsx/auto_runtime.c','    if((uintptr_t)registered_physical_end!=clean_end) return 0;\n','',
                            b'!ps3dk_prof__auto_fifo_end_known(&context,fifo+100,fifo+100)'),
    'end-known-foreign':('ringwrap','auto_rsx/auto_runtime.c',
                         '    if(!g_tls.slot1 || g_tls.acked || g_tls.retired ||\n'
                         '       a->owner_incarnation!=g_tls.incarnation || a->owner_generation!=g_tls.generation) return 0;\n',
                         '',b'known==0'),
    # a refusal found before any flush word was stored stops the ring on both sides: the sealed blocks never retire
    'halt-as-stop':('ringstop','auto_rsx/auto_runtime.c',
                    '    if(!a->ring_refusal) a->ring_refusal=reason;\n    auto_ring_halt(&a->ring);',
                    '    if(!a->ring_refusal) a->ring_refusal=reason;\n    auto_ring_stop(&a->ring);',b'RETIRED(a)==4'),
    # a held extent at seal not made a lower bound: FINAL claims completeness (checked on the capture)
    'seal-ignores-held':('ringlabel','auto_rsx/auto_runtime.c','            if(ring[3]) flags|=AUTO_LOWER_BOUND;\n','',None),
    # the init's io_address not recorded: the flip's CALL into the reserved prefix is refused
    'flip-unrecorded':('ringflip','auto_rsx/auto_runtime.c','    if(initial) init_record(gc,io_address,io_bytes,&io,&start);\n',
                       '    if(initial && 0) init_record(gc,io_address,io_bytes,&io,&start);\n',
                       b'!a->shadow.invalid && !a->ring_reject[2]'),
})
# checked on the capture after a clean run, not by an assertion of the application
CAPTURE_MUTANTS=('seal-ignores-held',)
RING_RUNTIME={'ringrun':0,'ringmeta':1,'ringshort':2,'ringfixture':3,'ringstop':4,'ringlabel':5,'ringflip':6,
              'ringsegment':7}
RING_WRAP=('ringwrap',*RING_RUNTIME)
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--case',choices=['noinit','fifoaccount','lease','init','race','ops','conflict','reset','stop','stopvisit','frames','clocks','clockcap','segments','extsegments','segcapacity','segmetatail','segbuild','segnested','callbracket','labels','tags','windows','windowseal','windowwhole','windownone','windowprebuild','windowhandoff','windowhandoffu','windowqueue','windowcallafter','flips','present','spanning','fifofail','fifopre','fifopost','seal','ring','ringwrap','ringrun','ringmeta','ringshort','ringfixture','ringstop','ringlabel','ringflip',
                                  'ringsegment'],default='noinit')
ap.add_argument('--fixture',type=Path,help='ringfixture, ringstop: copy the capture here')
ap.add_argument('--reader',type=Path,help='ring cases with a capture: the ps3dk-prof binary that must read it (exit 0)')
ap.add_argument('--mutant',choices=['fifo-frame-acquire','reject-late-end','ignore-global-stop','no-frame-boundary','clock-put',
                                  'fifo-publish','fifo-cancel','fifo-owner','fifo-freeze','retired-owner',
                                  'segment-silent','no-flip-frames','one-record-chunks','no-present-wait',
                                  'poll-wait','poll-scope-cadence','boundary-cadence','boundary-scope','external-silent','writer-on-segment',
                                  'replay-silent','recall-silent','moved-draw-silent','nested-silent','no-call-reserve','bracket-silent','label-silent','label-switch-silent','tag-silent','tag-at-call','range-end-silent','window-gate-off','frames-off-by-one','label-stride-4','label-sum','label-unknown-run','prebuild-silent',
                                  'owner-is-opener','queue-unreserved','opener-token-off','call-outside-gate',*RING_MUTANTS])
a=ap.parse_args()
operations=a.case in ('ops','conflict','reset','frames')
if a.mutant in RING_MUTANTS:assert a.case==RING_MUTANTS[a.mutant][0] and not a.sanitize
elif a.mutant:assert a.case=={'fifo-frame-acquire':'fifoaccount','reject-late-end':'stop','ignore-global-stop':'stopvisit','no-frame-boundary':'frames','clock-put':'clocks',
                          'fifo-publish':'fifopost','fifo-cancel':'fifopre','fifo-owner':'init',
                          'fifo-freeze':'init','retired-owner':'init','segment-silent':'segments','external-silent':'extsegments','writer-on-segment':'extsegments',
                          'no-flip-frames':'flips','one-record-chunks':'segments','no-present-wait':'flips',
                          'poll-wait':'present','poll-scope-cadence':'present',
                          'boundary-cadence':'spanning','boundary-scope':'spanning',
                          'replay-silent':'segbuild','recall-silent':'segbuild','moved-draw-silent':'segbuild','nested-silent':'segnested','no-call-reserve':'callbracket','bracket-silent':'callbracket','label-silent':'labels','label-switch-silent':'labels','tag-silent':'tags','tag-at-call':'tags','range-end-silent':'tags','window-gate-off':'windows','frames-off-by-one':'windows','label-stride-4':'labels','label-sum':'labels','label-unknown-run':'labels','prebuild-silent':'windowprebuild','owner-is-opener':'windowhandoff',
                          'queue-unreserved':'windowqueue','opener-token-off':'windowhandoffu',
                          'call-outside-gate':'windowcallafter'}[a.mutant] and not a.sanitize
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
    r=subprocess.run(cmd,capture_output=True,cwd=cwd,timeout=900 if a.case in RING_RUNTIME else 90,
                     env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=cmd,rc=r.returncode,cwd=str(cwd)))
    (out/'commands.json').write_bytes((json.dumps(commands,indent=2)+'\n').encode())
    if name=='run' and a.mutant=='fifo-frame-acquire':
        assert r.returncode==-6 and b'FIFO frame metadata requires attached publication' in r.stderr,(r.returncode,r.stderr.decode())
        print('fifo-frame-acquire rejected by pre-publication metadata assertion')
        raise SystemExit(0)
    if name=='run' and a.mutant and a.mutant not in ('clock-put','retired-owner','one-record-chunks','tag-silent','tag-at-call',
                                                     *CAPTURE_MUTANTS):
        expected=RING_MUTANTS[a.mutant][4] if a.mutant in RING_MUTANTS else {'reject-late-end':b'!g_tls.in_gpu && g_tls.acked',
                  'ignore-global-stop':b'g_tls.acked && !g_tls.in_gpu',
                  'no-frame-boundary':b'context.current==words+24',
                  'fifo-publish':b'g_fifo.state==FS_SEALED',
                  'fifo-cancel':b'auto_test_fifo_installs==1 && auto_test_fifo_removes==1',
                  'fifo-owner':b'p_gpu_fifo_owner(g_prof,g_fifo.domain,&context)==PS3DK_PROF_EPERM',
                  'fifo-freeze':b'ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_ESTOPPED',
                  'segment-silent':b'ticket && app==before+2',
                  'external-silent':b'ticket && app==before+2',
                  'writer-on-segment':b'ticket && app==before+2',
                  'no-flip-frames':b'counts[0]==i+1',
                  'no-present-wait':b'present[2]==1 && present[3]>0',
                  'poll-wait':b'p[0]==0 && p[1]==0',
                  'poll-scope-cadence':b'p[2]==0 && p[4]==0',
                  'boundary-cadence':b'p[2]==0 && p[3]==0 && p[5]==1',
                  'boundary-scope':b'p[0]==0 && p[1]==0 && p[6]==1',
                  'replay-silent':b're-CALL of a timed build sets the replay latch',
                  'recall-silent':b"a re-CALL counts the build's visits",
                  'moved-draw-silent':b'the part a moved draw ends in counts it',
                  'nested-silent':b'a CALL inside a draw bracket sets the replay latch',
                  'no-call-reserve':b'timed==504u-32u-16u-2u',
                  'bracket-silent':b'context.current==a+before',
                  'label-silent':b'l[0]==1 && l[1]==0 && l[3]==71',
                  'label-switch-silent':b'l[0]==2 && l[1]==2 && l[3]==0xffffffffu',
                  'range-end-silent':b'context.current==w0+6',
                  'window-gate-off':b'!zone() && !clear() && state()==0',
                  'prebuild-silent':b'h[0]==1 && h[1]==1',
                  'owner-is-opener':b'render_frames==5 && render_first==0',
                  'queue-unreserved':b'PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_EFULL',
                  'opener-token-off':b'opener_end1==PS3DK_PROF_OK && state()==0',
                  'call-outside-gate':b'(uint32_t *)(void *)context.current-before==5',
                  'frames-off-by-one':b'state()==0 && !zone() && !clear()',
                  'label-stride-4':b'l[0]==1 && l[3]==91 && l[5]==l[4]',
                  'label-sum':b'l[5]<l[4]',
                  'label-unknown-run':b'l[0]==1 && l[1]==1 && l[3]==91'}.get(a.mutant)
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
if a.case=='fifoaccount':flags+=['-DGPP_HOST_DEVMEM']
if a.case in ('clocks','clockcap'):flags+=['-DGPP_HOST_DEVMEM','-DTEST_CLOCK_FRAMES='+('256' if a.case=='clockcap' else '64')]
if a.case=='stopvisit':flags+=['-DTEST_STOP_VISIT=1']
if a.case in ('segments','extsegments','segcapacity','segmetatail','segbuild','segnested','callbracket','labels','tags','windows','windowseal','windowwhole','windownone','windowprebuild','windowhandoff','windowhandoffu','windowqueue','windowcallafter','flips','present','spanning'):flags+=['-DGPP_HOST_DEVMEM']
if a.case=='flips':
    # only the flip entry-point header: the SDK's own libc/pthread headers must not shadow the host ones
    (inc.parent/'cell/gcm').mkdir(parents=True)
    shutil.copyfile(root/'sdk/include/cell/gcm/ps3tc_gcm_flip.h',inc.parent/'cell/gcm/ps3tc_gcm_flip.h')
    sys_h=(root/'sdk/include/rsx/gcm_sys.h').read_text()
    start=sys_h.index('#ifndef PS3TC_GCM_LEGACY_SHIM_TU\n#include <cell/gcm/ps3tc_gcm_flip.h>')
    end=sys_h.index('}\n#endif\n#endif\n',start)+len('}\n#endif\n#endif\n')
    (inc.parent/'gcm_flip_aliases.h').write_text(sys_h[start:end],encoding='utf-8')
    flags+=['-DPS3TC_PROF_AUTO=1']
# the published per-draw segment room is the AUTO build's value only when the build defines PS3TC_PROF_AUTO
if a.case in ('seal','extsegments','segbuild','segnested','callbracket','tags'):flags+=['-DPS3TC_PROF_AUTO=1']
if a.case in ('windows','windowseal','windowwhole','windownone','windowprebuild','windowhandoff','windowhandoffu','windowqueue','windowcallafter'):
    flags+=['-DTEST_WINDOW_MODE='+str({'windows':0,'windowseal':1,'windowwhole':2,'windownone':4,'windowprebuild':5,
                                       'windowhandoff':6,'windowhandoffu':7,'windowqueue':8,'windowcallafter':9}[a.case])]
if a.case=='labels' and a.sanitize=='thread':
    # the header's host-only ordering fence (cellGcmReadLabelOrdered) is a seq_cst fence, which GCC warns about under
    # TSan; the runtime under test has no fence of its own
    flags+=['-Wno-tsan']
if a.case=='labels':
    # only the label query header: the SDK's own libc headers must not shadow the host ones
    (inc.parent/'cell/gcm').mkdir(parents=True)
    shutil.copyfile(root/'sdk/include/cell/gcm/ps3tc_gcm_labels.h',inc.parent/'cell/gcm/ps3tc_gcm_labels.h')
    if a.mutant=='label-stride-4':
        # the review's decoder: a 4-byte stride over a 1024-byte area (label 90's slot decodes as no label)
        hp=inc.parent/'cell/gcm/ps3tc_gcm_labels.h';code=hp.read_text()
        old='at < base + 16u * CELL_GCM_LABEL_COUNT && !((at - base) & 15u) ?\n                     (uint32_t)((at - base) / 16u)'
        assert code.count(old)==1
        hp.write_text(code.replace(old,'at < base + 4u * CELL_GCM_LABEL_COUNT && !((at - base) & 3u) ?\n                     (uint32_t)((at - base) / 4u)'))
    flags+=['-DPS3TC_PROF_AUTO=1']
if a.case=='spanning':flags+=['-DTEST_SPANNING=1']
if a.case in ('ring',*RING_WRAP):flags+=['-DGPP_HOST_DEVMEM','-DPS3DK_PROF_AUTO_RING=1']
if a.case in RING_RUNTIME:
    flags+=['-DTEST_RING_MODE='+str(RING_RUNTIME[a.case])]
    # the application's host stand-in for PUT's barrier is a seq_cst fence, which GCC warns about under TSan
    if a.sanitize=='thread':flags+=['-Wno-tsan']
if a.case in ('fifofail','fifopre','fifopost'):flags+=['-DTEST_FIFO_POINT='+str({'fifofail':0,'fifopre':3,'fifopost':4}[a.case])]
if a.sanitize:flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
if a.sanitize=='address':
    # Same host TLS relaxation constraint as arcs-lifecycle-test: the linker
    # rewrites ADD to LEA while the UBSan null check consumes its flags. All TLS
    # is in this executable; local-exec prevents that relaxation. Keep all
    # sanitizer checks enabled, including null checks.
    flags+=['-ftls-model=local-exec','-fno-omit-frame-pointer']
cc=os.environ.get('CC','cc')
sources=['core/prof_core.c','core/prof_drain.c','core/prof_version.c','pal/pal_host.c', 'rolling/roll_file.c', 'rolling/roll_retention_inspect.c', 'rolling/roll_store.c',
         'spu/psp_ppu.c','spu/psp_spu.c','spu/ext_runtime.c','spu/psp_elf.c','spu/psp_host_emul.c',
         'rsx/gpu_runtime.c','rsx/prg.c','rsx/gpu_port_gcm.c','rsx/rsxcap.c',
         'fifo/fifo_runtime.c','fifo/fifo_writer.c',
         'auto_rsx/auto_runtime.c','auto_rsx/auto_startup.c','auto_rsx/auto_config.c',
         'auto_rsx/auto_wire.c','auto_rsx/auto_writer.c','auto_rsx/auto_queue.c',
         'auto_rsx/auto_commands.c','auto_rsx/auto_completion.c','auto_rsx/auto_frames.c','auto_rsx/auto_clock.c',
         'auto_rsx/auto_ring.c','auto_rsx/auto_ring_commands.c','auto_rsx/auto_ring_layout.c']
objects=[]
for rel in sources:
    source=src/rel;obj=out/(source.stem+'.o')
    if a.mutant=='fifo-frame-acquire' and rel=='fifo/fifo_runtime.c':
        code=source.read_text()
        old='    if (LD_ACQ(&g_fifo.state) != FS_ATTACHED) return;\n'
        assert code.count(old)==1
        source=out/'fifo_runtime.c';source.write_bytes(code.replace(old,'').encode())
    elif a.mutant in ('fifo-publish','fifo-cancel','fifo-freeze') and rel=='fifo/fifo_runtime.c':
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
        code=source.read_text();old='if(!before || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return;\n    if(status) {'
        assert code.count(old)==1
        new='if(!before || !(a=owner_capture(NULL,0))) return;\n    if(status) {'
        code=code.replace(old,new)
        old2='flip_status_begin(void)\n{\n    auto_capture *a;\n    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return 0;\n    return observation_tb()|1u;'
        assert code.count(old2)==1
        code=code.replace(old2,'flip_status_begin(void)\n{\n    auto_capture *a;\n    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0))) return 0;\n    return observation_tb()|1u;')
        source=out/'auto_runtime.c';source.write_bytes(code.encode())
    elif a.mutant=='one-record-chunks' and rel=='auto_rsx/auto_runtime.c':
        code=source.read_text();old='enum { AUTO_OPS_BATCH=64 };';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'enum { AUTO_OPS_BATCH=1 };').encode())
    elif a.mutant=='nested-silent' and rel=='auto_rsx/auto_runtime.c':
        # review finding reintroduced: a CALL inside an open draw bracket returns before any accounting
        code=source.read_text();old='    if(g_tls.in_gpu) {\n';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    if(g_tls.in_gpu) {return;\n').encode())
    elif a.mutant in ('replay-silent','recall-silent','moved-draw-silent') and rel=='auto_rsx/auto_runtime.c':
        # CALL completeness reintroduced as silent: a replay, a re-CALL's draws, or a moved draw's destination
        old,new={'replay-silent':('if(s->reports) ST_REL(&a->segment_replay,1u);','(void)0;'),
                 'recall-silent':('(void)auto_frames_unrepresented(&a->coverage,s->visits);return;','return;'),
                 'moved-draw-silent':('if(d && d!=s) {history_add(&d->visits,1);history_add(&d->refused,1);history_add(&d->other_refused,1);}','(void)d;')}[a.mutant]
        code=source.read_text();assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant=='no-call-reserve' and rel=='auto_rsx/auto_queue.c':
        # covered draws may take the pairs kept for whole-CALL brackets
        code=source.read_text();old='    return keep;\n}';assert code.count(old)==1
        source=out/'auto_queue.c';source.write_bytes(code.replace(old,'    (void)keep;return 0;\n}').encode())
    elif a.mutant in ('owner-is-opener','queue-unreserved') and rel=='core/prof_core.c':
        # the review findings reintroduced: the opener owns the window; admission ignores the queue's room
        old,new={'owner-is-opener':('            p->win.owner_slot1 = owner;\n            p->win.owner_inc = owner_inc;',
                                    '            p->win.owner_slot1 = t->slot1;\n            p->win.owner_inc = t->incarnation;(void)owner_inc;'),
                 'queue-unreserved':('    } else if (!win_room(p)) {','    } else if (0 && !win_room(p)) {')}[a.mutant]
        code=source.read_text();assert code.count(old)==1,a.mutant
        source=out/'prof_core.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant=='opener-token-off' and rel=='core/prof_core.c':
        # the review finding reintroduced: an unregistered opener has no identity once another thread owns the window
        code=source.read_text();old='!(t->win_token && t->win_token == p->win.opener_token)';assert code.count(old)==1
        source=out/'prof_core.c';source.write_bytes(code.replace(old,'!(0 && t->win_token == p->win.opener_token)').encode())
    elif a.mutant=='call-outside-gate' and rel=='auto_rsx/auto_runtime.c':
        # the review finding reintroduced: the post-CALL observer runs while no window is open
        code=source.read_text();old='    if(!p_window_gate(g_prof)) {\n        /* A CALL while no capture window';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    if(0 && !p_window_gate(g_prof)) {\n        /* A CALL while no capture window').encode())
    elif a.mutant=='prebuild-silent' and rel=='auto_rsx/auto_runtime.c':
        # the review finding reintroduced: a segment draw outside a window leaves no build accounting
        code=source.read_text();old='        else unobserved_build(ctx,call);\n';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'        else (void)unobserved_build;\n').encode())
    elif a.mutant in ('window-gate-off','frames-off-by-one') and rel=='core/prof_core.c':
        # zones recorded outside a window; a FRAMES(n) window held one frame too long
        old,new={'window-gate-off':('    if (!p_window_gate(p))\n        return h;','    if (0)\n        return h;'),
                 'frames-off-by-one':('fid - p->win.first >= p->win.n','fid - p->win.first > p->win.n')}[a.mutant]
        code=source.read_text();assert code.count(old)==1,a.mutant
        source=out/'prof_core.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant in ('tag-silent','tag-at-call','range-end-silent') and rel=='auto_rsx/auto_runtime.c':
        # tags not recorded on ring and segment draws; a segment draw's tag taken at its CALL; no end report
        old,new={'tag-silent':('    e->op.tag=a->tag;e->op.fp_offset=a->fp_ring;\n    if(a->coverage.active) {e->op.flags=1;',
                               '    e->op.fp_offset=a->fp_ring;\n    if(a->coverage.active) {e->op.flags=1;'),
                 'tag-at-call':('e->op=b->op;e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;e->op.call=call;',
                                'e->op=b->op;e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;e->op.call=call;e->op.tag=a->tag;'),
                 'range-end-silent':('    segment_report(ctx,report+1);\n    {\n        uint64_t id=e->op.id;',
                                     '    {\n        uint64_t id=e->op.id;')}[a.mutant]
        code=source.read_text();assert code.count(old)==1,a.mutant
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant in ('label-sum','label-unknown-run') and rel=='auto_rsx/auto_runtime.c':
        # the one-label total replaced by the sum across labels, or an unidentified address forming runs again
        old,new={'label-sum':('    if(a->label_sum[i]>f->label_one) f->label_one=a->label_sum[i];\n','    f->label_one=f->label_cadence;\n'),
                 'label-unknown-run':('    if(label==0xffffffffu) return; /* no identity: no run */\n','')}[a.mutant]
        code=source.read_text();assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant in ('label-silent','label-switch-silent') and rel=='auto_rsx/auto_runtime.c':
        # label polls unobserved, or a poll of another label continuing the open run
        old,new={'label-silent':('    if(!before || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return;\n    if(a->label_open && a->label_at!=label)',
                                 '    return;\n    if(a->label_open && a->label_at!=label)'),
                 'label-switch-silent':('    if(a->label_open && a->label_at!=label) label_close(a,0);\n',
                                        '    if(a->label_open && a->label_at!=label) a->label_at=label;\n')}[a.mutant]
        code=source.read_text();assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,new).encode())
    elif a.mutant=='bracket-silent' and rel=='auto_rsx/auto_runtime.c':
        # no whole-CALL bracket: a build that is not fully timed gets no CALL span
        code=source.read_text();old='    if(!(s=segment_find(a,segment)) || s->called || s->bracket || !s->refused) return;\n';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    return;\n').encode())
    elif a.mutant=='writer-on-segment' and rel=='auto_rsx/auto_runtime.c':
        # review finding reintroduced: the in-flight emitter is matched by the segment's writer, which a part close clears
        code=source.read_text();old='a->segment_active_writer==(void *)context';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'a->segment_active->writer==(void *)context').encode())
    elif a.mutant=='external-silent' and rel=='auto_rsx/auto_runtime.c':
        # no writer inference: an application context writing an external segment stays a foreign visit
        code=source.read_text();old='    if((s=external_writer(a,ctx))) return segment_begin(a,s,ctx,call,caller);\n';assert code.count(old)==1
        source=out/'auto_runtime.c';source.write_bytes(code.replace(old,'    (void)external_writer;\n').encode())
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
        if a.mutant=='ignore-global-stop':
            # the visit outside a capture window acknowledges the stop on its own path: ignored there too
            gate='        if(LD_ACQ(&g_prof->stop_latch)) p_observe_stop(g_prof);\n'
            assert code.count(gate)==1
            code=code.replace(gate,'        if(0) p_observe_stop(g_prof);\n')   # the line stays: an else may follow it
        source=out/'auto_runtime.c';source.write_bytes(code.encode())
    if a.mutant in RING_MUTANTS and rel==RING_MUTANTS[a.mutant][1]:
        code=source.read_text();old,new=RING_MUTANTS[a.mutant][2:4];assert code.count(old)==1,a.mutant
        source=out/source.name;source.write_bytes(code.replace(old,new).encode())
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
#include "gpu_port_gcm.h"
unsigned auto_test_fifo_installs,auto_test_fifo_removes;
int auto_test_fifo_fail,auto_test_fifo_native;
volatile uint32_t auto_test_fifo_put=4096;
uint64_t ps3tc_fifo_observe_suppressed(void) {return 0;}
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
int ps3tc_fifo_observe_account_snapshot(struct _gcmCtxData *c,uint32_t *current,uint32_t *put) {
 if(c!=context || !auto_test_fifo_native)return -1;
 gpp_ctx *gc=(gpp_ctx *)(void *)c;
 *current=4096+(uint32_t)(gc->current-gc->begin)*4;*put=auto_test_fifo_put;return 0;
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
if a.case=='fifoaccount':application=Path(__file__).with_name('auto-fifo-accounting-test.c').read_text()
if a.case=='lease':application=Path(__file__).with_name('auto-lease-test.c').read_text()
if a.case=='init':application=Path(__file__).with_name('auto-attach-test.c').read_text()
if a.case=='race':application=Path(__file__).with_name('auto-attach-race-test.c').read_text()
if a.case in ('fifofail','fifopre','fifopost'):application=Path(__file__).with_name('auto-fifo-test.c').read_text()
if operations:application=Path(__file__).with_name('auto-operations-test.c').read_text()
if a.case in ('clocks','clockcap'):application=Path(__file__).with_name('auto-clock-runtime-test.c').read_text()
if a.case in ('stop','stopvisit'):application=Path(__file__).with_name('auto-operations-stop-test.c').read_text()
if a.case=='segments':application=Path(__file__).with_name('auto-segment-runtime-test.c').read_text()
if a.case=='extsegments':application=Path(__file__).with_name('auto-external-segment-test.c').read_text()
if a.case=='segcapacity':application=Path(__file__).with_name('auto-segment-capacity-test.c').read_text()
if a.case=='segmetatail':application=Path(__file__).with_name('auto-segment-metadata-tail-test.c').read_text()
if a.case=='segbuild':application=Path(__file__).with_name('auto-segment-build-frame-test.c').read_text()
if a.case=='segnested':application=Path(__file__).with_name('auto-segment-call-in-bracket-test.c').read_text()
if a.case in ('windows','windowseal','windowwhole','windownone','windowprebuild','windowhandoff','windowhandoffu','windowqueue','windowcallafter'):application=Path(__file__).with_name('auto-window-runtime-test.c').read_text()
if a.case=='tags':application=Path(__file__).with_name('auto-tag-runtime-test.c').read_text()
if a.case=='labels':application=Path(__file__).with_name('auto-label-poll-runtime-test.c').read_text()
if a.case=='callbracket':application=Path(__file__).with_name('auto-segment-call-bracket-test.c').read_text()
if a.case=='flips':application=Path(__file__).with_name('auto-flip-runtime-test.c').read_text()
if a.case in ('present','spanning'):application=Path(__file__).with_name('auto-present-runtime-test.c').read_text()
if a.case=='seal':application=Path(__file__).with_name('auto-seal-test.c').read_text()
if a.case=='ring':application=Path(__file__).with_name('auto-ring-attach-test.c').read_text()
if a.case=='ringwrap':application=Path(__file__).with_name('auto-ring-wrap-test.c').read_text()
if a.case in RING_RUNTIME:application=Path(__file__).with_name('auto-ring-runtime-test.c').read_text()
(out/'main.c').write_text(application,encoding='utf-8')
extra=[]
if a.case in RING_WRAP:
    stub=out/'stub'
    for rel,text in {
        'rsx/gcm_sys.h':'#pragma once\n#include <stdint.h>\ntypedef uint32_t u32;\nstruct _gcmCtxData;\n'
            'typedef int32_t (*gcmContextCallback)(struct _gcmCtxData *,uint32_t);\n'
            'typedef struct _gcmCtxData { u32 *begin,*end,*current; gcmContextCallback callback; } gcmContextData;\n'
            'typedef struct { volatile u32 put,get,ref; } gcmControlRegister;\n'
            'void *gcmGetControlRegister(void);\nint32_t gcmAddressToOffset(void *,u32 *);\n',
        'sys/timer.h':'#pragma once\nint sys_timer_usleep(unsigned long);\n',
        'sys/systime.h':'#pragma once\n#include <stdint.h>\nuint64_t sysGetTimebaseFrequency(void);\n',
        'sys/lv2_types.h':'#pragma once\n',
        'ppu-asm.h':'#pragma once\n#include <stdint.h>\n#define lv2_fn_to_callback_ea(f) ((uintptr_t)(f))\n',
        'ppu_host.h':'#pragma once\n#include <stdint.h>\nuint64_t auto_test_timebase(void);\n'
            '#define __builtin_ppc_get_timebase() auto_test_timebase()\n'}.items():
        (stub/rel).parent.mkdir(parents=True,exist_ok=True);(stub/rel).write_text(text,encoding='utf-8')
    code=(root/'sdk/libgcm_cmd/src/ps3tc_fifo_wrap.c').read_text()
    old='__asm__ __volatile__ ("sync" ::: "memory");';assert code.count(old)==1
    code=code.replace(old,'__atomic_thread_fence(__ATOMIC_SEQ_CST);')
    if a.mutant in RING_MUTANTS and RING_MUTANTS[a.mutant][1]=='ps3tc_fifo_wrap.c':
        old,new=RING_MUTANTS[a.mutant][2:4];assert code.count(old)==1,a.mutant;code=code.replace(old,new)
    (out/'ps3tc_fifo_wrap.c').write_text(code,encoding='utf-8')
    wflags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-DPS3TC_PROF_AUTO=1','-include',stub/'ppu_host.h',
            '-I'+str(stub),'-I'+str(root/'sdk/libgcm_cmd/include'),'-I'+str(root/'sdk/libgcm_cmd/src')]
    wflags+=[f for f in flags if f.startswith('-fsanitize') or f.startswith('-fno-') or f.startswith('-ftls')]
    # the host stand-in for the PPU sync is a seq_cst fence (owner thread only), which GCC warns about under TSan
    if a.sanitize=='thread':wflags+=['-Wno-tsan']
    run('compile-ps3tc_fifo_wrap',[cc,*wflags,'-c',out/'ps3tc_fifo_wrap.c','-o',out/'ps3tc_fifo_wrap.o'])
    extra=[out/'ps3tc_fifo_wrap.o']
# every case but the window API ones records through a whole-run window opened right after init (the shared helper)
whole_run=[] if a.case in ('windows','windowseal','windowwhole','windownone','windowprebuild','windowhandoff','windowhandoffu','windowqueue','windowcallafter') else [
    root/'tests/host/ps3dk-prof-common/prof_whole_run.c','-Wl,--wrap=ps3dk_prof_init','-Wl,--wrap=ps3dk_prof_init_ex']
run('link',[cc,*flags,out/'main.c',out/'bridge.c',*extra,*whole_run,'-Wl,-u,__ps3dk_prof_auto_startup_anchor',out/'libauto.a','-o',out/'app'])
capture_dir=out/'capture';capture_dir.mkdir()
cmd=[out/'app']
if a.sanitize=='thread':cmd=['setarch','x86_64','-R',*cmd]
result=run('run',cmd,capture_dir)
if a.case in ('noinit','seal'):
    assert result.stderr==b'', result.stderr
if a.case=='lease':
    assert result.stdout==b'AUTO/manual shared reservation PASS\n',result.stdout
    print(result.stdout.decode(),end='')
    raise SystemExit(0)
if a.case in RING_RUNTIME:
    passed={'ringrun':b'PASS','ringmeta':b'AUTO ring metadata-full without a frame-tail latch PASS',
            'ringshort':b'AUTO ring short sink retires nothing PASS','ringfixture':b'AUTO ring fixture PASS',
            'ringstop':b'AUTO ring producer-only stop drains the sealed blocks PASS',
            'ringlabel':b'AUTO ring stopped with held extents PASS',
            'ringflip':b'AUTO ring flip CALL into the reserved prefix PASS',
            'ringsegment':b'AUTO ring CALL into an application segment refused PASS'}[a.case]
    assert result.stdout.endswith(passed+bytes([10])),result.stdout
    print(result.stdout.decode(),end='')
    if a.case in ('ringrun','ringfixture','ringstop','ringlabel','ringflip'):
        import importlib.util
        spec=importlib.util.spec_from_file_location('ring_capture',Path(__file__).with_name('auto_ring_capture.py'))
        check=importlib.util.module_from_spec(spec);spec.loader.exec_module(check)
        files=list(capture_dir.glob('*.prof'));assert len(files)==1,files
        data=files[0].read_bytes()
        try:
            if a.case=='ringstop':
                # blocks 1-4 timed and retired, block 5's ten records incomplete and its extent held (LOWER_BOUND); the
                # decoder's first rejection: the binding header at the offset the application wrote it
                summary=check.verify(data,pending_tail=10,held_extents=1)
                at=int(result.stdout.split(b'header written at +')[1].split()[0],16)
                assert summary['rejected']==(0x00042000,at,8) and summary['blocks_retired']==4,summary
            elif a.case=='ringlabel':
                # nothing read: every ring record incomplete, both extents held (LOWER_BOUND), no decoder rejection
                count=sum(1 for o in check.ops(data) if o['flags']&128)
                summary=check.verify(data,pending_tail=count,held_extents=2)
                assert summary['blocks_retired']==0 and summary['rejected']==(0,0,0),summary
            elif a.case=='ringflip':
                # every draw timed across both flips, every extent retired, nothing rejected: an exact FINAL
                summary=check.verify(data)
                assert summary['rejected']==(0,0,0) and summary['held']==0 and summary['ring_records']==280,summary
                assert not summary['status_flags']&2 and summary['blocks_retired']==4,summary
            else:
                summary=check.verify(data,pending_tail=5 if a.case=='ringrun' else 0)
        except AssertionError as e:
            if a.mutant not in CAPTURE_MUTANTS:raise
            print(a.mutant+' rejected by the capture check: '+str(e));raise SystemExit(0)
        assert a.mutant not in CAPTURE_MUTANTS,a.mutant+' survived the capture check'
        print(json.dumps(summary,sort_keys=True))
        if a.reader:
            # the branch reader reads it (exit 0, not MALFORMED); a stopped ring reads as a lower bound
            r=run('reader',[a.reader,'auto',files[0]],capture_dir)
            text=r.stdout.decode()
            if a.case in ('ringstop','ringlabel'):assert 'FINAL (lower bounds)' in text,text
            if a.case=='ringflip':assert 'FINAL' in text and 'lower bound' not in text,text
            if a.case=='ringstop':
                assert 'ring decoder rejected header 0x00042000 at +%#x: ' % at in text,text
            print('reader exit 0: '+next(l for l in text.splitlines() if 'FINAL' in l))
        if a.case in ('ringfixture','ringstop') and a.fixture:shutil.copyfile(files[0],a.fixture)
    raise SystemExit(0)
if a.case in ('ring','ringwrap'):
    assert result.stdout.endswith((b'AUTO ring attach, partition, budgets and label reservation PASS' if a.case=='ring' else
                                   b'AUTO ring physical tail and native pre-wrap flush PASS')+bytes([10])),result.stdout
    print(result.stdout.decode(),end='')
    raise SystemExit(0)
if a.case=='segmetatail':
    assert result.returncode==0 and result.stdout==b'AUTO metadata exhaustion latches the frame tail PASS\n',result.stdout
    print(result.stdout.decode(),end='')
    raise SystemExit(0)
if a.case=='callbracket':
    assert result.returncode==0 and result.stdout==b'AUTO whole-CALL brackets: 454 timed draws, 32 brackets from the reserve, the 33rd refused PASS\n',result.stdout
if a.case=='segcapacity':
    assert result.returncode==0 and result.stdout.startswith(b'AUTO segment capacity: 24 segments, 100 draws per build') and result.stdout.endswith(b'untimed PASS\n'),result.stdout
    print(result.stdout.decode(),end='')
    raise SystemExit(0)
assert result.stdout==(b'AUTO initial attach and override without device writes PASS\n' if a.case=='init'
                       else b'AUTO native FIFO accounting PASS\n' if a.case=='fifoaccount'
                       else b'AUTO actual marker producer and cumulative completion PASS\n' if operations
                       else b'AUTO in-flight owner seal retains claim and lower bound PASS\n' if a.case=='stop'
                       else b'AUTO stopped owner visit acknowledges without marker PASS\n' if a.case=='stopvisit'
                       else b'AUTO actual bounded scheduled clock capture PASS\n' if a.case in ('clocks','clockcap')
                       else b'AUTO segment timing and frame marks PASS\n' if a.case=='segments'
                       else b'AUTO external segment timing PASS\n' if a.case=='extsegments'
                       else b'AUTO segment build frames and CALL completeness PASS\n' if a.case=='segbuild'
                       else b'AUTO segment CALL inside a draw bracket PASS\n' if a.case=='segnested'
                       else b'AUTO whole-CALL brackets: 454 timed draws, 32 brackets from the reserve, the 33rd refused PASS\n' if a.case=='callbracket'
                       else b'AUTO tags, fragment programs, ranges and timed ranges PASS\n' if a.case=='tags'
                       else b'AUTO capture windows PASS\n' if a.case in ('windows','windowseal')
                       else b'AUTO whole-run window PASS\n' if a.case=='windowwhole'
                       else b'AUTO no window PASS\n' if a.case=='windownone'
                       else b'AUTO pre-window segment build PASS\n' if a.case=='windowprebuild'
                       else b'AUTO window frame-thread handoff PASS\n' if a.case in ('windowhandoff','windowhandoffu')
                       else b'AUTO window queue PASS\n' if a.case=='windowqueue'
                       else b'AUTO CALL outside a window PASS\n' if a.case=='windowcallafter'
                       else b'AUTO label-poll cadence PASS\n' if a.case=='labels'
                       else b'AUTO flip-driven frames PASS\n' if a.case=='flips'
                       else b'AUTO presentation evidence PASS\n' if a.case in ('present','spanning')
                       else b'AUTO attach/seal blocked owner refused without publication PASS\n' if a.case=='race'
                       else b'AUTO FIFO refusal and unpublished cleanup PASS\n' if a.case in ('fifofail','fifopre','fifopost')
                       else b'AUTO app seal PASS\n' if a.case=='seal'
                       else b'application complete\n'),result.stdout
files=list(capture_dir.glob('*.prof'));assert len(files)==1,files
data=files[0].read_bytes()
assert data[:8]==b'PS3DKPRF'
# file_flags: 195 plus bit 9 (capture windows) on every capture
assert struct.unpack_from('<HHI',data,8)==(1,0,195|512),struct.unpack_from('<HHI',data,8)
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
if a.case=='fifoaccount':
    rows=[struct.unpack('<HHIIIII7Q',b) for k,b in chunks if k==43]
    assert len(rows)==3,rows
    assert rows[0][9:12]==(216,208,2) and rows[0][6]==0,rows
    assert rows[1][9:12]==(144,104,0) and rows[1][6]==0,rows
    assert rows[2][9:12]==(0,0,0) and rows[2][6]&1,rows
    print('AUTO native FIFO accounting wire PASS')
    raise SystemExit(0)
auto=[(k,b) for k,b in chunks if 34<=k<=38]
OP=160 # OPS record width of this writer (CONFIG 63/11): 128 bytes plus the build tail
def build_tail(q):
    # build frame, build owner, build flags, reserved
    return struct.unpack_from('<QIHH',q,128)
if a.case=='fifopost':
    assert [k for k,b in auto]==[34,37]
    assert struct.unpack_from('<HHI',auto[-1][1])==(1,11,1),'unacknowledged AUTO owner'
    assert not any(31<=k<=33 for k,b in chunks),'cancelled FIFO never published'
    assert chunks[-1][0]==6
    assert struct.unpack_from('<I',chunks[-1][1])[0]&1,'owner did not acknowledge shutdown'
    print('AUTO accepted before seal, unpublished FIFO cancelled, owner remains UNKNOWN PASS')
    raise SystemExit(0)
if a.case=='segbuild':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11) and struct.unpack_from('<I',config,60)[0]==OP
    owner=struct.unpack_from('<I',config,16)[0]
    records=[]
    for k,b in auto:
        if k!=35:continue
        assert struct.unpack_from('<I',b,12)[0]==OP
        for i in range(struct.unpack_from('<I',b,8)[0]):
            q=b[16+OP*i:16+OP*(i+1)]
            records.append(dict(kind=struct.unpack_from('<H',q,12)[0],flags=struct.unpack_from('<H',q,14)[0],
                frame=struct.unpack_from('<Q',q,16)[0],segment=struct.unpack_from('<I',q,92)[0],
                call=struct.unpack_from('<I',q,116)[0],tail=build_tail(q)))
    seg=[(r['flags'],r['frame'],r['segment'],r['call'],r['tail']) for r in records if r['flags']&64]
    # part ordinals in open order: A 1, S 2, X 3, C 4, D 5, E 6, R 7, P 8 (U is never tracked).
    # A: built in 0, recorded under its CALL in 1; E: one draw built in each of 0 and 1, both recorded in 1;
    # R: built in 2, its CALL refused: an INCOMPLETE drop without a CALL token or call ordinal.
    assert seg==[(67,1,1,1,(0,owner,1,0)),(67,1,6,2,(0,owner,1,0)),(67,1,6,2,(1,owner,1,0)),
                 (64|4|16,0,7,0,(2,owner,1,0))],seg
    assert all(r['tail']==(0,0,0,0) for r in records if not r['flags']&64)
    rows=[b for k,b in auto if k==36];assert len(rows)==3 and all(len(b)==480 for b in rows)
    unrep=[struct.unpack_from('<I',b,380)[0] for b in rows]
    assert unrep==[0,3,0xffffffff],unrep
    rflags=[struct.unpack_from('<H',b,2)[0] for b in rows]
    assert not rflags[0]&4 and not rflags[1]&4 and rflags[2]&4,rflags # the saturated row is SATURATED
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    assert fields[1]&1024,('SEGMENT_REPLAY',fields[1])
    assert fields[1]&(1|2|4|64)==1|2|4|64,('FINAL, LOWER_BOUND, SATURATED, SEGMENT_QUARANTINE',fields[1])
    print('real capture: segment build frames per draw, CALL completeness counted, replay and saturation PASS')
    raise SystemExit(0)
if a.case=='segnested':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11)
    rows=[b for k,b in auto if k==36];assert rows and all(len(b)==480 for b in rows)
    unrep=[struct.unpack_from('<I',b,380)[0] for b in rows]
    assert unrep[0]==3,unrep # first CALL inside a draw, a later replay, a replay inside a draw: one visit each
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    assert fields[1]&1024,('SEGMENT_REPLAY',fields[1])
    print('real capture: CALLs inside a draw bracket counted and the replay latch set PASS')
    raise SystemExit(0)
if a.case=='callbracket':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11)
    owner=struct.unpack_from('<I',config,16)[0]
    calls=[]
    for k,b in auto:
        if k!=35:continue
        for i in range(struct.unpack_from('<I',b,8)[0]):
            q=b[16+OP*i:16+OP*(i+1)]
            if struct.unpack_from('<H',q,12)[0]==5:
                calls.append((struct.unpack_from('<H',q,14)[0],struct.unpack_from('<Q',q,16)[0],
                              struct.unpack_from('<7I',q,64),struct.unpack_from('<2I',q,92)[0:1]+struct.unpack_from('<I',q,116),
                              build_tail(q)))
    assert len(calls)==32,len(calls)
    # TOKEN|RAW_VALID in frame 0, never SEGMENT; part 1 (ordinal 2): 455 draws, 1 refused for report capacity in frame 0
    assert all(f==3 and fr==0 and so==(0,0) for f,fr,a_,so,t in calls),calls[:2]
    assert calls[0][2]==(455,1,2,2,1,0,0) and calls[0][4]==(0,owner,1,0),calls[0]
    # parts 2..31 (ordinals 3..32) and part 0 rebuilt (ordinal 1): one refused draw each, call ordinals 4..34
    assert [c[2][2] for c in calls[1:]]==list(range(3,33))+[1],[c[2] for c in calls]
    assert all(c[2][0:2]==(1,1) and c[2][4]==1 and c[4]==(0,owner,1,0) for c in calls[1:])
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    assert fields[1]&1024,('SEGMENT_REPLAY',fields[1])
    print('real capture: 32 whole-CALL records with their build frames, replay latched PASS')
    raise SystemExit(0)
if a.case in ('windows','windowseal','windowwhole','windownone','windowprebuild','windowhandoff','windowhandoffu','windowqueue','windowcallafter'):
    # the WINDOW records (chunks 41/42), the zone and frame records (chunk 5), the AUTO records and rows
    wins=[]
    for k,b in chunks:
        if k==41:
            assert len(b)==32 and struct.unpack_from('<HH',b)==(1,0)
            wid,desc,first=struct.unpack_from('<IIQ',b,4);src,kind=b[20],b[21];n=struct.unpack_from('<I',b,24)[0]
            assert src==1 and b[22:24]==bytes(2) and b[28:]==bytes(4)
            wins.append(['B',wid,first,kind,n])
        elif k==42:
            assert len(b)==32 and struct.unpack_from('<HH',b)==(1,0) and b[9:12]==bytes(3) and b[20:]==bytes(12)
            wid,=struct.unpack_from('<I',b,4);last,=struct.unpack_from('<Q',b,12)
            wins.append(['E',wid,b[8],last])
    kinds={}
    zone_frames=[]
    for k,b in chunks:
        if k!=5:continue
        n=struct.unpack_from('<I',b,4)[0]
        for i in range(n):
            r=b[8+32*i:8+32*(i+1)]
            kinds[r[0]]=kinds.get(r[0],0)+1
    ops=[];rows=[]
    for k,b in auto:
        if k==35:
            for i in range(struct.unpack_from('<I',b,8)[0]):
                q=b[16+OP*i:16+OP*(i+1)]
                ops.append((struct.unpack_from('<H',q,12)[0],struct.unpack_from('<Q',q,16)[0],struct.unpack_from('<H',q,14)[0]))
        if k==36:rows.append(struct.unpack_from('<Q',b,16)[0])
    frames=kinds.get(4,0)
    if a.case in ('windows','windowseal'):
        end_reason=4 if a.case=='windowseal' else 3
        assert wins==[['B',1,1,1,0],['E',1,1,1],['B',2,3,2,3],['E',2,2,5],['B',3,6,3,0],['E',3,1,6],
                      ['B',4,8,1,0],['E',4,end_reason,8]],wins
        # every frame mark is kept (run-wide numbering); zones only inside windows: frames 1 (two), 3, 4, 5, 6, 8
        assert frames==9 and kinds.get(1,0)==7 and kinds.get(3,0)==7,kinds
        # AUTO: rows and FRAME marks only for frames a window held from their boundary; draws only inside windows
        assert rows==[1,3,4,5,8],rows
        clears=[o for o in ops if o[0]==3]
        assert len(clears)==7,ops
        assert {o[1] for o in clears if o[2]&1}=={1,3,4,5,8} and sum(1 for o in clears if not o[2]&1)==1,clears
        print('real capture: windows at frame boundaries, FRAMES(3), SECTION, EBUSY and the '+('SEAL' if a.case=='windowseal' else 'SHUTDOWN')+' close; nothing heavy outside PASS')
    elif a.case=='windowprebuild':
        # one window (frame 1); both CALLs inside it accounted, the pre-window draw never timed
        assert wins==[['B',1,1,1,0],['E',1,1,1]],wins
        print('real capture: pre-window and boundary-crossing segment builds accounted PASS')
    elif a.case=='windowhandoff':
        # the render thread's frames 0..4, ended by it
        assert wins==[['B',1,0,1,0],['E',1,1,4]],wins
        print('real capture: the frame-marking thread owns the window (registered opener) PASS')
    elif a.case=='windowhandoffu':
        # the opener ends the first window while its owner lives (frames 0..4); the second (from frame 5) after the
        # owner retired, at the owner's last frame
        assert wins==[['B',1,0,1,0],['E',1,1,4],['B',2,5,1,0],['E',2,1,5]],wins
        print('real capture: the frame-marking thread owns the window and the unregistered opener may end it PASS')
    elif a.case=='windowcallafter':
        # the whole-run window held frame 0, the second window frame 2; no segment record is a timed (raw-valid)
        # draw: the CALL after END and the re-CALL in the new window are unrepresented
        assert wins==[['B',1,0,1,0],['E',1,1,0],['B',2,2,1,0],['E',2,1,2]],wins
        assert not any(o[2]&64 and o[2]&2 for o in ops),ops
        print('real capture: a CALL after its window closed and a later re-CALL record no timed draw PASS')
    elif a.case=='windowqueue':
        # nine windows, every END after its own BEGIN: the refused BEGIN recorded nothing
        assert wins==[r for i in range(1,10) for r in (['B',i,i-1 if i<9 else 8,1,0],['E',i,1,i-1 if i<9 else 8])],wins
        print('real capture: a full window queue refuses the BEGIN; no WINDOW_END without its BEGIN PASS')
    elif a.case=='windownone':
        # no trigger: no window, no zone, no AUTO row or draw record; the frame marks are kept
        assert wins==[] and frames==5 and not kinds.get(1) and not kinds.get(3),(wins,kinds)
        assert rows==[] and not any(o[0] in (1,2,3,4) for o in ops),(rows,ops)
        print('real capture: no trigger, 5 frame marks and nothing heavy PASS')
    else:
        assert wins==[["B",1,0,1,0],["E",1,3,4]],wins
        # the same heavy records with and without the whole-run window
        assert frames==5 and kinds.get(1,0)==5 and kinds.get(3,0)==5,kinds
        assert rows==[0,1,2,3,4] and sum(1 for o in ops if o[0]==3)==5,(rows,ops)
        print('real capture: the whole-run window holds 5 frames, 5 zones and 5 timed clears PASS')
    raise SystemExit(0)
if a.case=='tags':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11) and struct.unpack_from('<I',config,60)[0]==OP==160
    recs=[]
    for k,b in auto:
        if k!=35:continue
        for i in range(struct.unpack_from('<I',b,8)[0]):
            q=b[16+OP*i:16+OP*(i+1)]
            recs.append(dict(kind=struct.unpack_from('<H',q,12)[0],flags=struct.unpack_from('<H',q,14)[0],
                frame=struct.unpack_from('<Q',q,16)[0],args=struct.unpack_from('<7I',q,64),
                tag=struct.unpack_from('<I',q,144)[0],fp=struct.unpack_from('<I',q,148)[0],
                depth=struct.unpack_from('<I',q,152)[0],r=struct.unpack_from('<I',q,156)[0]))
    assert all(r['r']==0 for r in recs)
    ring=[(r['tag'],r['fp']) for r in recs if r['kind']==3 and not r['flags']&64]
    seg=[(r['tag'],r['fp']) for r in recs if r['flags']&64]
    if a.mutant=='tag-silent':
        assert ring[0][0]!=0xa,ring
        print('tag-silent rejected: ring draws lose their tag',ring[:2]);raise SystemExit(0)
    if a.mutant=='tag-at-call':
        assert seg[0][0]==0xc and seg!=[(0xa,0),(0xa,0x2000)],seg
        print('tag-at-call rejected: segment draws take the CALL tag',seg);raise SystemExit(0)
    calls=[(r['tag'],r['fp']) for r in recs if r['kind']==5]
    rng=[(r['frame'],r['depth'],r['args'][1],r['tag']) for r in recs if r['kind']==6]
    # ring clears: 0xa, 0xb with the ring's program, the nested-range clears under 0xd, frame 1's timed clears
    assert ring[:4]==[(0xa,0),(0xb,0x1000),(0xd,0x1000),(0xd,0x1000)],ring
    assert ring[4:]==[(0xe,0x1000)]*3,ring
    # part 0's draws keep the builder's tag 0xa and the part's own program (set after its first draw)
    assert seg==[(0xa,0),(0xa,0x2000)],seg
    # each CALL record carries the tag at its CALL (part 1's draw was left untimed outside the timed ranges, so its
    # CALL is bracketed too)
    assert calls==[(0xc,0x1000),(0xe,0x1000)],calls
    # inner then outer, the four nested ranges unwinding, nine ranges of the reserve in frame 1, all tag 0xd
    assert [x[:3] for x in rng]==[(0,2,0),(0,1,0),(0,4,0),(0,3,0),(0,2,0),(0,1,0)]+[(1,1,0)]*9,rng
    assert all(x[3]==0xd for x in rng),rng
    rows=[b for k,b in auto if k==36];assert len(rows)==2 and all(len(b)==480 for b in rows)
    tail=[struct.unpack_from('<3QII',b,416) for b in rows]
    assert tail[0]==(0,0,0,3,0),tail[0]
    # the boundary range, seven ranges past the reserve, and the timed and inner ranges once it is spent
    assert tail[1]==(0,0,3,10,0),tail[1]
    print('real capture: tags, fragment programs, ranges and the outside-timed reason PASS')
    raise SystemExit(0)
if a.case=='labels':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11)
    rows=[b for k,b in auto if k==36]
    assert len(rows)>=3 and all(len(b)==480 for b in rows),[len(b) for b in rows]
    block=[struct.unpack_from('<4IQQ',b,384) for b in rows[:3]]
    # frame 0: two complete runs (labels 70 and 80: several), two broken, the one-label total (u64@408) below the
    # sum; frame 1: one complete run (label 90 through the address form), two broken (the 90 -> 91 switch and an
    # unidentified poll), one spanning, the one-label total equal to the sum; frame 2: one spanning run, nothing
    # complete
    assert block[0][:4]==(2,2,0,0xffffffff) and 0<block[0][5]<block[0][4],block[0]
    assert block[1][:4]==(1,2,1,91) and block[1][5]==block[1][4],block[1]
    assert block[2][:4]==(0,0,1,0) and block[2][4]==0 and block[2][5]==0,block[2]
    print('real capture: label-poll blocks in 416-byte rows PASS')
    raise SystemExit(0)
if a.case=='extsegments':
    records=[]
    for k,b in auto:
        if k!=35:continue
        n=struct.unpack_from('<I',b,8)[0]
        for i in range(n):
            q=b[16+OP*i:16+OP*(i+1)]
            records.append((struct.unpack_from('<H',q,12)[0],struct.unpack_from('<H',q,14)[0],
                            struct.unpack_from('<I',q,92)[0],struct.unpack_from('<I',q,116)[0]))
    # kind, flags, segment ordinal, call ordinal: FRAME begin mark, part 1 (array, indexed) completed by call 1,
    # part 2 (clear) by call 2, FRAME end mark; the untimed and foreign draws leave no record
    tails=[]
    for k,b in auto:
        if k!=35:continue
        for i in range(struct.unpack_from('<I',b,8)[0]):tails.append(build_tail(b[16+OP*i:16+OP*(i+1)]))
    seg=[r for r in records if r[1]&64]
    owner=struct.unpack_from('<I',[b for k,b in auto if k==34][0],16)[0]
    # every segment draw was built in frame 0; nothing else carries a tail
    assert [t for r,t in zip(records,tails) if r[1]&64]==[(0,owner,1,0)]*4,tails
    assert all(t==(0,0,0,0) for r,t in zip(records,tails) if not r[1]&64),tails
    # the mid-draw switch leaves no record (its pair is quarantined); part D's draw is completed by call 3
    assert [(r[0],r[2],r[3]) for r in seg]==[(1,1,1),(2,1,1),(3,2,2),(3,4,3)],records
    assert all((r[1]&3)==3 for r in seg),records
    assert [r[0] for r in records if not r[1]&64]==[4,4],records
    print('AUTO external CALL segments: two parts through one writer context, records exact PASS')
    raise SystemExit(0)
if a.case=='segments':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11),'CONFIG kind/form masks of the build-frame writer'
    assert struct.unpack_from('<I',config,60)[0]==OP,'CONFIG record width'
    records=[]
    for k,b in auto:
        if k!=35:continue
        n=struct.unpack_from('<I',b,8)[0]
        for i in range(n):
            q=b[16+OP*i:16+OP*(i+1)]
            records.append(dict(id=struct.unpack_from('<Q',q,0)[0],kind=struct.unpack_from('<H',q,12)[0],
                flags=struct.unpack_from('<H',q,14)[0],frame=struct.unpack_from('<Q',q,16)[0],
                raw=struct.unpack_from('<2Q',q,40),args=struct.unpack_from('<7I',q,64),
                segment=struct.unpack_from('<I',q,92)[0],call=struct.unpack_from('<I',q,116)[0],
                serials=struct.unpack_from('<2I',q,104),published=struct.unpack_from('<Q',q,120)[0],
                tail=build_tail(q)))
    owner=struct.unpack_from('<I',config,16)[0]
    # segment draws name the frame that built them: part A's and the dropped part B's in frame 0, A's rebuild in 1
    assert [r['tail'] for r in records]==[(0,0,0,0)]*2+[(0,owner,1,0)]*3+[(0,0,0,0)]*2+[(1,owner,1,0),(0,0,0,0)],\
        [r['tail'] for r in records]
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
        assert len(b)==480 and struct.unpack_from('<H',b,2)[0]==3|64
        assert struct.unpack_from('<Q',b,16)[0]==frame
        assert struct.unpack_from('<Q',b,40)[0]==unsupported,('unsupported_form_visits',frame,b[40:48])
        rows=[struct.unpack_from('<12Q',b,64+96*k) for k in range(3)]
        assert [r[:2] for r in rows]==rows_expected and all(not any(r[2:]) for r in rows),rows
        assert struct.unpack_from('<I',b,380)[0]==0,'every CALL of this capture is represented'
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    assert fields[1]==1|2|64|512,('FINAL, LOWER_BOUND (a held quarantined pair), SEGMENT_QUARANTINE, FRAMES_EXPLICIT',fields[1])
    assert fields[3:6]==(9,9,9) and fields[17]==1,fields # nine records; one quarantined pair held
    print('real capture: segment builds timed per CALL, a never-called build quarantined, FRAME marks PASS')
    raise SystemExit(0)
if a.case=='flips':
    config=[b for k,b in auto if k==34][0]
    assert struct.unpack_from('<2I',config,24)==(1023,11)
    rows=[b for k,b in auto if k==36];assert len(rows)==22,len(rows)
    assert all(len(b)==480 for b in rows)
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
            q=b[16+OP*i:16+OP*(i+1)]
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
    assert struct.unpack_from('<2I',config,24)==(1023,11)
    rows=[b for k,b in auto if k==36]
    n=3 if a.case=='present' else 5
    assert len(rows)==n and all(len(b)==480 for b in rows),[len(b) for b in rows]
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
        # the flip-wait scope block at 448: where frame 2's two scopes ran, inside the frame, in order, their elapsed
        # times summing to the row's ticks; no scope, no block
        at=struct.unpack_from('<4Q',rows[2],448);fb,fe=struct.unpack_from('<2Q',rows[2],24)
        assert fb<=at[0]<=at[1]<=at[2]<=at[3]<=fe and (at[1]-at[0])+(at[3]-at[2])==r[2]['ticks'],(at,fb,fe)
        assert 20*MS<=at[1]-at[0]<21*MS,at
        assert all(struct.unpack_from('<4Q',b,448)==(0,0,0,0) for b in rows[:2])
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
        # bracketed anchors (127/11): the device ran both pairs before the first drain poll, so no poll saw the
        # label below 2k-1 (label_first 0, lower bound = issue time) and the completing poll saw 2k
        assert struct.unpack_from('<2I',b,64)==(0,2*(i+1))
        assert struct.unpack_from('<QIIQ',b,72)==(schedule[i],owner,0,after-before)
    coverage=[b for k,b in auto if k==36];assert len(coverage)==frames
    assert all(struct.unpack_from('<H',b,2)[0]==3|64 for b in coverage)
    assert all(not any(struct.unpack_from('<36Q',b,64)) for b in coverage)
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    marks=2*frames # a begin and an end FRAME mark per frame, each one serial pair
    assert fields[:3]==(1,1|512,1) and fields[3:]==(marks,marks,marks,0,0,frames,0,0,0,len(schedule),len(rows),
        len(schedule)-len(rows),0,2*marks,0),fields
    kinds=[struct.unpack_from('<HH',b,16+OP*i+12) for k,b in auto if k==35 for i in range(struct.unpack_from('<I',b,8)[0])]
    assert len(kinds)==marks and all(k==(4,3) for k in kinds),'every FRAME mark raw-valid'
    assert chunks[-1][0]==6
    print('real capture: '+str(len(rows))+' CLOCK facts, '+str(frames)+' closed frames, '+str(len(schedule)-len(rows))+' lost anchors and FINAL accounting PASS')
    raise SystemExit(0)
if a.case in ('stop','stopvisit'):
    assert [k for k,b in auto]==[34,37]
    fields=struct.unpack_from('<HHI15Q',auto[-1][1])
    active=int(a.case=='stop')
    assert fields[:3]==(1,11 if active else 1,1),fields
    # capture windows: the stopped visit comes after shutdown closed the whole-run window, so it is not counted (no
    # unassociated visit); the in-flight draw of 'stop' was admitted inside the window
    assert fields[3:]==(active,active,0,0,active,0,0,active,0,0,0,0,0,0,active),fields
    assert chunks[-1][0]==6
    print('real capture: '+a.case+' owner acknowledgement and retained-resource accounting PASS')
    raise SystemExit(0)
if operations:
    assert auto[0][0]==34 and auto[-1][0]==37
    ops=[]
    for k,b in auto:
        if k!=35:continue
        assert struct.unpack_from('<HHI',b)==(1,0,1)
        n=struct.unpack_from('<I',b,8)[0];assert len(b)==16+n*OP
        ops += [b[16+i*OP:16+(i+1)*OP] for i in range(n)]
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
            assert len(b)==480 and struct.unpack_from('<HHI',b)==(1,(19 if i==1 else 3)|64,1)
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
assert len(config)==176 and len(status)==128
assert struct.unpack_from('<IIQ',config,160)[2]==0
if a.case=='init':
    assert struct.unpack_from('<HHI',config)==(1,0,1)
    assert struct.unpack_from('<IIIIII',config,8)==(1026,1,1,1,1023,11)
    assert struct.unpack_from('<I',config,60)[0]==OP
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
assert config[96:128]==bytes([42])+bytes(31) and config[128:160]==bytes([99])+bytes(31)
# the tag writer's reserves (u32@160 whole-CALL, u32@164 ranges; refused CONFIGs carry none): an attached capture of
# the default pool keeps 32 and 16
if struct.unpack_from('<H',config,2)[0]==0:
    pairs=struct.unpack_from('<I',config,48)[0]
    assert struct.unpack_from('<IIQ',config,160)==((32,16,0) if pairs>=128 else (pairs//4,pairs//8,0)),config[160:]
else:
    assert config[160:]==bytes(16)
clocks=[b for k,b in chunks if k==2];assert len(clocks)==1
assert struct.unpack_from('<Q',config,64)[0]==struct.unpack_from('<Q',clocks[0],12)[0]>0
assert struct.unpack_from('<HHI',status)==(1,1,1) and status[8:]==bytes(120)
assert chunks[-1][0]==6, 'complete capture trailer'
if a.case=='seal':
    err_src=out/'main_err.c'
    err_src.write_text('#include <assert.h>\n#include "prof_test_hooks.h"\nint main(void) { ps3dk_prof_test_hooks.join_fail=1; return 0; }\n',encoding='utf-8')
    err_app=out/'app_err'
    run('link-err',[cc,*flags,err_src,out/'bridge.c','-Wl,-u,__ps3dk_prof_auto_startup_anchor',out/'libauto.a','-o',err_app])
    err_dir=out/'capture_err';err_dir.mkdir()
    err_cmd=[err_app]
    if a.sanitize=='thread':err_cmd=['setarch','x86_64','-R',*err_cmd]
    err_res=run('run-err',err_cmd,err_dir)
    assert err_res.returncode==0
    assert b"ps3prof: automatic finalization failed; inspect capture validity\n" in err_res.stderr
print('host attach/config/declaration lifecycle PASS' if a.case=='init' else
      'automatic FIFO refusal: CONFIG reason8 + zero FINAL STATUS PASS' if a.case=='fifofail' else
      'automatic capture: application seal and clean destructor PASS' if a.case=='seal' else
      'profiler-free host application: automatic CONFIG reason10 + zero FINAL STATUS PASS')
