#include "auto_internal.h"
#include "auto_commands.h"
#include "../fifo/fifo_internal.h"
#include "../rsx/gpu_internal.h"
#include <string.h>

/* The core admits one capture per process. CONFIG remains mutable only through
 * the owner-side initialization/override window; its encoder cannot freeze it.
 * This control state also exists when no GCM initialization is ever observed,
 * so selecting the archive never silently loses that diagnostic. */
static auto_config_store configuration;
static auto_writer writer;
/* Only the configuring owner writes these while CONFIG is WRITING. The
 * drainer reads capture only after acquiring FROZEN with an attached CONFIG. */
static auto_capture *capture;
static gpp_ctx *clean_context;
static uintptr_t clean_begin, clean_current, clean_end;

static int label_valid(uint32_t n) { return n==64 || (n>=69 && n<=254); }

/* The defaults (no options: the automatic attach at cellGcmInit) must stay inside the profiler's reservation of
 * <cell/gcm/ps3tc_gcm_labels.h>: labels 240..247 and reports 0..511 or 1024..2047. An override is the
 * application's own choice of indices and keeps the general rule above. */
#define AUTO_PROFILER_LABEL_FIRST 240u
#define AUTO_PROFILER_LABEL_LAST 247u
#define AUTO_PROFILER_REPORT_AUTO_FIRST 1024u
#define AUTO_PROFILER_REPORT_AUTO_LAST 2047u
#define AUTO_PROFILER_REPORT_LAST 511u
static int profiler_label(uint32_t n) { return n>=AUTO_PROFILER_LABEL_FIRST && n<=AUTO_PROFILER_LABEL_LAST; }
static int profiler_reports(uint32_t first, uint32_t count)
{
    uint64_t last=(uint64_t)first+count-1u;
    if(!count) return 0;
    return last<=AUTO_PROFILER_REPORT_LAST ||
           (first>=AUTO_PROFILER_REPORT_AUTO_FIRST && last<=AUTO_PROFILER_REPORT_AUTO_LAST);
}
static int defaults_reserved(const ps3dk_prof_auto_options *o)
{
    return profiler_label(o->operation_label) && profiler_label(o->clock_label) &&
           profiler_reports(o->report_first,o->report_count);
}

static int layout(const ps3dk_prof_auto_options *o, size_t *records, size_t *frames, size_t *total)
{
    uint64_t n;
    if(o->size!=sizeof *o || o->report_count<18 || o->report_count>1024 || (o->report_count&1) ||
       o->report_first>2048-o->report_count || !label_valid(o->operation_label) ||
       !label_valid(o->clock_label) || o->operation_label==o->clock_label ||
       !o->metadata_records || o->metadata_records>1024 || !o->frame_records || o->frame_records>256 ||
       !o->max_bytes || o->max_bytes>2097152) return 0;
    /* Every multiplier is bounded above before these widened expressions.
     * Include padding in the published allocation, not just symbol payloads. */
    n=((uint64_t)sizeof(auto_capture)+127u)&~UINT64_C(127);
    *records=(size_t)n;
    n+=(uint64_t)o->metadata_records*sizeof(auto_queue_entry);
    n=(n+127u)&~UINT64_C(127);
    *frames=(size_t)n;
    n+=(uint64_t)o->frame_records*sizeof(auto_wire_frame);
    n=(n+127u)&~UINT64_C(127);
    if(n>SIZE_MAX || n>o->max_bytes) return 0;
    *total=(size_t)n;
    return 1;
}

static const gpp_dev *device(void)
{
#ifdef PS3DK_PROF_PAL_LV2
    return &gpp_dev_gcm;
#elif defined(PS3DK_PROF_TEST_HOOKS)
    return (const gpp_dev *)ps3dk_prof_test_hooks.gpu_gpp_dev;
#else
    return NULL;
#endif
}

static int attach(ps3dk_prof_gcm_ctx *ctx, const ps3dk_prof_auto_options *options,
                  uint32_t *instance, uint32_t *domain, int initial, int32_t application_rc)
{
    const ps3dk_prof_auto_options defaults=PS3DK_PROF_AUTO_OPTIONS_INIT;
    const ps3dk_prof_auto_options *o=options ? options:&defaults;
    auto_wire_config config={0};
    auto_capture *next=NULL,*prior;
    prof *p;
    gpp_ctx *gc=(gpp_ctx *)(void *)ctx;
    uint32_t claim=0,lease=0,id=0,reason=0;
    size_t records=0,frames=0,bytes=0;
    int rc=PS3DK_PROF_EINVAL,locked=0,fifo_prepared=0;
    if(instance) *instance=0;
    if(domain) *domain=0;
    if(!instance || !domain) return rc;
    if(g_tls.in_drain || g_tls.in_gpu) return PS3DK_PROF_EBUSY;
    p=p_ready() ? g_prof:NULL;
    if(!p) return PS3DK_PROF_ENOTINIT;
    if(!layout(o,&records,&frames,&bytes)) return rc;
    if(!options && !defaults_reserved(o)) return rc; /* defaults never take application indices */
    /* Become a participant before owning mutable configuration or entering a
     * device accessor. Shutdown's owner inventory can then see this setup. */
    if(!application_rc && gc && !g_tls.slot1 && p->cfg.auto_register)
        (void)ps3dk_prof_thread_begin("automatic-gcm");
    claim=auto_config_begin(&configuration);
    if(!claim) return PS3DK_PROF_ESTOPPED;
    prior=capture;
    if(initial && clean_context) {
        auto_config_abort(&configuration,claim);
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
        return PS3DK_PROF_ESTOPPED;
    }
    if(application_rc) { reason=AUTO_GCM_INIT_FAILED;goto fail; }
    if(p->cfg.mode!=PS3DK_PROF_MODE_THREAD || !p->drain_thread_started) {
        reason=AUTO_NO_INDEPENDENT_DRAIN;rc=PS3DK_PROF_EPERM;goto fail;
    }
    if(!gc) { reason=AUTO_UNSUPPORTED_CONTEXT;goto fail; }
    if(!g_tls.slot1 || g_tls.acked || g_tls.retired) {
        reason=AUTO_NO_OWNER;rc=PS3DK_PROF_ETHREAD;goto fail;
    }
    if(!initial && (gc!=clean_context || (prior && !pal_owner_is_self(&prior->owner)))) {
        reason=AUTO_LEASE_UNVERIFIED;rc=PS3DK_PROF_EPERM;goto fail;
    }
    if(!initial && ((uintptr_t)(void *)gc->begin!=clean_begin ||
       (uintptr_t)(void *)gc->current!=clean_current || (uintptr_t)(void *)gc->end!=clean_end)) {
        reason=AUTO_PRIOR_WRITES_UNKNOWN;goto fail;
    }
    g_tls.in_gpu=1; /* cached device accessors cannot re-enter managed GPU APIs */
    next=pal_alloc(bytes);
    if(!next) { reason=AUTO_ALLOCATION_FAILED;rc=PS3DK_PROF_ENOMEM;goto fail; }
    memset(next,0,bytes);
    if(pal_owner_get(&next->owner)!=0) {reason=AUTO_NO_OWNER;rc=PS3DK_PROF_ETHREAD;goto fail;}
    if(gpp_prepare(&next->operations_port,device(),gc,o->report_first,o->report_count,o->operation_label)!=PRG_OK ||
       gpp_prepare(&next->clock_port,device(),gc,o->report_first,16,o->clock_label)!=PRG_OK) {
        reason=AUTO_LEASE_UNVERIFIED;goto fail;
    }
    next->context=gc;next->owner_slot=g_tls.slot1-1;
    next->owner_incarnation=g_tls.incarnation;next->owner_generation=g_tls.generation;
    next->allocated=bytes;next->frame_capacity=o->frame_records;
    next->cadence_bound=p->tb_freq/(1000000u/AUTO_CADENCE_BOUND_US);
    next->frames=(auto_wire_frame *)((uint8_t *)next+frames);
    (void)auto_frames_init(&next->coverage,next->frames,o->frame_records,1,g_tls.incarnation);
    if(!auto_clock_init(&next->clocks,1,1,o->report_first,p->tb_freq)) {
        reason=AUTO_UNSUPPORTED_CONTEXT;goto fail;
    }
    (void)auto_queue_init(&next->operations,(auto_queue_entry *)((uint8_t *)next+records),
                          o->metadata_records,(o->report_count-16)/2);
    for(uint32_t i=0;i<AUTO_SEGMENT_POOL;i++) next->segment_pool[i].next=i+1<AUTO_SEGMENT_POOL ? i+1:AUTO_SEGMENT_NONE;
    next->segment_free=0;next->segment_pending=AUTO_SEGMENT_NONE;
    gpp_bind(&next->operations_device,&next->operations_port);
    gpp_bind(&next->clock_device,&next->clock_port);
    if(p_ext_lock_at(p)!=0) {reason=AUTO_STOPPED;rc=PS3DK_PROF_ESTOPPED;goto fail;}
    locked=1;
    rc=p_auto_lease_claim(p,gc,o->report_first,o->report_count,o->operation_label,o->clock_label,&id,&lease);
    if(rc!=0) {reason=rc==PS3DK_PROF_ESTOPPED ? AUTO_STOPPED:AUTO_RESOURCE_CONFLICT;goto fail;}
    next->lease_ticket=lease;
    /* CONFIG's WRITING claim protects the candidate, not the extension mutex.
     * The FIFO prepare can overlap bounded shutdown; never park its owner with
     * that mutex held against the independent drainer. */
    rc=p_ext_unlock_site(p,U_GPU_ATTACH);locked=0;
    if(rc!=0) {reason=AUTO_STOPPED;rc=PS3DK_PROF_ESTOPPED;goto fail;}
    if(prior) next->fifo_instance=prior->fifo_instance;
    else {
        rc=p_fifo_auto_prepare(id,(void *)gc);
        if(rc!=PS3DK_PROF_OK) {
            reason=rc==PS3DK_PROF_ESTOPPED ? AUTO_STOPPED:AUTO_UNSUPPORTED_CONTEXT;
            goto fail;
        }
        fifo_prepared=1;next->fifo_instance=1;
    }
    config.instance=1;config.domain=id;config.epoch=1;config.owner=g_tls.incarnation;config.generation=1;
    config.report_first=o->report_first;config.report_count=o->report_count;
    config.op_label=o->operation_label;config.clock_label=o->clock_label;
    config.pairs=(o->report_count-16)/2;config.metadata=o->metadata_records;config.frames=o->frame_records;
    config.allocated=bytes;config.ceiling=o->max_bytes;
    capture=next;
    if(initial) {
        clean_context=gc;clean_begin=(uintptr_t)(void *)gc->begin;
        clean_current=(uintptr_t)(void *)gc->current;clean_end=(uintptr_t)(void *)gc->end;
    }
    if(!auto_config_commit(&configuration,claim,&config)) {
        capture=NULL;
        if(fifo_prepared) p_fifo_auto_cancel();
        else if(prior && prior->fifo_instance) (void)ps3dk_prof_fifo_detach(prior->fifo_instance);
        if(p_ext_lock_at(p)==0) {
            (void)p_auto_lease_drop(p,lease);
            (void)p_ext_unlock_site(p,U_GPU_ATTACH);
        }
        pal_free(next);pal_free(prior);
        g_tls.in_gpu=0;
        return PS3DK_PROF_ESTOPPED;
    }
    pal_free(prior); /* no observation or device write can precede this override */
    g_tls.in_gpu=0;
    if(fifo_prepared && p_fifo_auto_publish()!=PS3DK_PROF_OK) {
        /* AUTO may already be frozen and visible to the sealing drainer.
         * Keep its capture/lease alive; only the unpublished FIFO is cancelled. */
        return PS3DK_PROF_ESTOPPED;
    }
    *instance=1;*domain=id;
    return PS3DK_PROF_OK;
fail:
    if(lease) {
        if(!locked && p_ext_lock_at(p)==0) locked=1;
        if(locked) (void)p_auto_lease_drop(p,lease);
    }
    if(locked) (void)p_ext_unlock_site(p,U_GPU_ATTACH);
    pal_free(next);
    g_tls.in_gpu=0;
    if(initial) {
        config.instance=1;config.reason=reason;config.application_rc=application_rc;
        (void)auto_config_commit(&configuration,claim,&config);
    } else auto_config_abort(&configuration,claim);
    return rc;
}

int ps3dk_prof_auto_attach(ps3dk_prof_gcm_ctx *context, const ps3dk_prof_auto_options *options,
                          uint32_t *instance, uint32_t *domain)
{
    return attach(context,options,instance,domain,0,0);
}

void ps3dk_prof__auto_gcm_init_result(ps3dk_prof_gcm_ctx *context, uint32_t command_bytes,
                                    uint32_t io_bytes, const void *io_address, int32_t rc)
{
    uint32_t instance,domain;
    /* Only the selected SDK init wrapper calls this, after extent admission.
     * No original application result is changed by an observer refusal. */
    (void)command_bytes;(void)io_bytes;(void)io_address;
    (void)attach(context,NULL,&instance,&domain,1,rc);
}

/* A mutable candidate is protected by the same CONFIG claim as attachment.
 * Only the registered emitting owner may turn its first observation into a
 * freeze. Afterwards capture and the resource identity are immutable; foreign
 * callers cannot touch an owner queue or close an override window. */
static auto_capture *owner_capture(gpp_ctx *ctx, int observe)
{
    const auto_wire_config *c=auto_config_read(&configuration);
    auto_capture *a;
    uint32_t claim;
    if(!g_tls.slot1 || g_tls.acked || g_tls.retired || g_tls.in_drain) return NULL;
    if(!c) {
        if(!p_ready() || !observe || g_tls.in_gpu) return NULL;
        claim=auto_config_begin(&configuration);
        if(!claim) return NULL;
        a=capture;
        int owned=a && (!ctx || a->context==ctx) && a->owner_incarnation==g_tls.incarnation &&
                  a->owner_generation==g_tls.generation && pal_owner_is_self(&a->owner);
        auto_config_abort(&configuration,claim);
        if(!owned || !auto_config_freeze(&configuration)) return NULL;
        c=auto_config_read(&configuration);
    }
    if(!c || c->reason) return NULL;
    a=capture;
    return a && (!ctx || a->context==ctx) && a->owner_incarnation==g_tls.incarnation &&
           a->owner_generation==g_tls.generation && pal_owner_is_self(&a->owner) ? a:NULL;
}

/* Called only after the FIFO callback validated its owner TLS. This boundary
 * uses atomics and TLS only: no lock, allocation, PAL identity syscall or device
 * access in the wrap callback. Foreign/manual contexts leave AUTO unchanged. */
void ps3dk_prof__auto_fifo_boundary(void *ctx)
{
    uint32_t claim;
    auto_capture *a;
    if(auto_config_read(&configuration))return;
    claim=auto_config_begin(&configuration);
    if(!claim)return;
    a=capture;
    int owned=a && a->context==ctx && a->owner_incarnation==g_tls.incarnation &&
              a->owner_generation==g_tls.generation;
    auto_config_abort(&configuration,claim);
    if(owned)(void)auto_config_freeze(&configuration);
}

static uint64_t observation_tb(void)
{
#ifdef PS3DK_PROF_PAL_LV2
    uint64_t t;
    __asm__ __volatile__("sync\n\tmftb %0\n\tisync" : "=r"(t) : : "memory");
    return t;
#else
    return p_tb();
#endif
}

static void increment(auto_capture *a,uint64_t *v)
{
    uint64_t old=LD_ACQ(v);
    for(;;) {
        if(old==UINT64_MAX) {ST_REL(&a->saturated,1u);return;}
        if(CAS_ACQREL(v,&old,old+1u)) return;
    }
}

static int initialize_device(auto_capture *a)
{
    const auto_wire_config *c=auto_config_read(&configuration);
    if(a->device_initialized) return 1;
    if(!c || !p_auto_lease_mark(g_prof,a->lease_ticket)) return 0;
    a->operations_device.write_label(a->operations_device.ctx,c->op_label,0);
    a->clock_device.write_label(a->clock_device.ctx,c->clock_label,0);
    a->operations_device.sync(a->operations_device.ctx);
    a->device_initialized=1;return 1;
}

static uint64_t clock_tb(void *user) {(void)user;return observation_tb();}
static int clock_live(void *user)
{
    auto_capture *a=user;
    if((uintptr_t)(void *)a->context->begin!=clean_begin || (uintptr_t)(void *)a->context->end!=clean_end) {
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);return 0;
    }
    return !LD_ACQ(&g_prof->stop_latch) && auto_config_can_admit(&configuration) &&
           !auto_config_flags(&configuration);
}
static int clock_emit(void *user,uint32_t report,uint32_t serial)
{
    auto_capture *a=user;
    uintptr_t begin=(uintptr_t)(void *)a->context->begin,current=(uintptr_t)(void *)a->context->current,
              end=(uintptr_t)(void *)a->context->end;
    /* Six words, no reserve callback or special tail. This boundary may refuse
     * an anchor; it cannot expand or wrap the application's command extent. */
    if(begin>current || current>end || (current&3u) || end-current<24 || !initialize_device(a)) return 0;
    a->clock_device.write_timer(a->clock_device.ctx,report,PRG_POISON);
    a->clock_device.sync(a->clock_device.ctx);
    return a->clock_device.reserve_pair(a->clock_device.ctx,report,a->clock_port.label,serial)==0;
}
static void clock_flush(void *user) {auto_capture *a=user;a->clock_device.flush(a->clock_device.ctx);}
static uint32_t clock_label(void *user) {auto_capture *a=user;return a->clock_device.read_label(a->clock_device.ctx,a->clock_port.label);}
static uint64_t clock_timer(void *user,uint32_t report) {auto_capture *a=user;return a->clock_device.read_timer(a->clock_device.ctx,report);}
static void clock_sync(void *user) {auto_capture *a=user;a->clock_device.sync(a->clock_device.ctx);}
static void sample_clock(auto_capture *a,uint64_t frame)
{
    const auto_clock_port port={a,clock_tb,clock_live,clock_emit,clock_flush,clock_label,clock_timer,clock_sync};
    g_tls.in_gpu=1;
    (void)auto_clock_sample(&a->clocks,&port,a->owner_incarnation,frame);
    if(a->clocks.invalid) {auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);}
    g_tls.in_gpu=0;
}

void ps3dk_prof__auto_resource_write(struct _gcmCtxData *context,uint32_t kind,uint32_t index)
{
    const auto_wire_config *c;
    auto_capture *a;
    uint32_t claim=0;
    (void)context; /* the report/label storage is shared across contexts */
    if(!p_ready() || (kind!=1 && kind!=2)) return;
    c=auto_config_read(&configuration);
    if(!c) {
        claim=auto_config_begin(&configuration);
        if(!claim) {
            /* A public writer during an in-flight mapping/override cannot be
             * checked against an immutable range. Stop that candidate rather
             * than guessing which reservation the command will affect. */
            auto_config_invalidate(&configuration,AUTO_STATUS_CONFLICT);
            return;
        }
    }
    a=capture;
    if(a && (!c || !c->reason)) {
        uint32_t first=a->operations_port.report_first,count=a->operations_port.report_count;
        int conflict=kind==1 ? (index>=2048 || (index>=first && index-first<count)):
                     (index==a->operations_port.label || index==a->clock_port.label);
        if(conflict) {
            increment(a,&a->conflicts);
            auto_queue_stop(&a->operations);
            auto_config_invalidate(&configuration,AUTO_STATUS_CONFLICT);
        }
    }
    if(claim) auto_config_abort(&configuration,claim);
}

static auto_segment *segment_find(auto_capture *a,const void *ctx)
{
    for(uint32_t i=0;i<a->segments_used;i++) if(a->segments[i].ctx==ctx) return &a->segments[i];
    return NULL;
}

/* Built operations that will never complete through our label: commit each as
 * an INCOMPLETE record (so accounting stays exact) whose pair the drainer then
 * quarantines; with no metadata room, quarantine the pair directly. Their
 * reports may still be written if the buffer is ever executed. */
/* The segment pool: one free list; a build's operations are a list in emission order. */
static uint32_t pool_take(auto_capture *a)
{
    uint32_t i=a->segment_free;
    if(i!=AUTO_SEGMENT_NONE) {a->segment_free=a->segment_pool[i].next;a->segment_pool[i].next=AUTO_SEGMENT_NONE;}
    return i;
}
static void pool_give(auto_capture *a,uint32_t i)
{
    a->segment_pool[i].next=a->segment_free;a->segment_free=i;
}
static void segment_append(auto_capture *a,auto_segment *s,uint32_t i)
{
    a->segment_pool[i].next=AUTO_SEGMENT_NONE;
    if(s->count) a->segment_pool[s->tail].next=i; else s->head=i;
    s->tail=i;s->count++;
}
/* Return a build's operations to the pool. */
static void segment_release(auto_capture *a,auto_segment *s)
{
    uint32_t i=s->count ? s->head:AUTO_SEGMENT_NONE;
    while(i!=AUTO_SEGMENT_NONE) {uint32_t next=a->segment_pool[i].next;pool_give(a,i);i=next;}
    s->count=0;
}

static void segment_drop(auto_capture *a,auto_segment *s)
{
    for(uint32_t i=s->count ? s->head:AUTO_SEGMENT_NONE;i!=AUTO_SEGMENT_NONE;i=a->segment_pool[i].next) {
        auto_segment_op *b=&a->segment_pool[i];
        auto_queue_entry *e=auto_queue_claim_held(&a->operations,b->pair,0);
        if(e) {
            uint64_t id=e->op.id;uint32_t sb=e->op.serial_begin,se=e->op.serial_end;
            e->op=b->op;e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;
            e->op.flags=(uint16_t)((e->op.flags&(1u|AUTO_OP_SEGMENT))|4u);
            ST_REL(&a->admitted,id);
            auto_queue_commit(&a->operations);
        } else auto_queue_quarantine_pair(&a->operations,b->pair);
        ST_REL(&a->segment_quarantine,1u);
    }
    segment_release(a,s);
}

/* cellGcmSegmentBegin/End (observed build). Owner only: another thread's
 * segment stays unregistered, and the owner never sees its emitters. Begin on
 * a segment whose previous build was never called drops that build. */
void ps3dk_prof__auto_segment_open(struct _gcmCtxData *context)
{
    auto_capture *a;
    auto_segment *s;
    if(!context || !p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,1))) return;
    if((void *)context==(void *)a->context) return;
    if(!(s=segment_find(a,context))) {
        if(a->segments_used==AUTO_SEGMENTS_OPEN || a->segment_ordinal==UINT32_MAX) {
            increment(a,&a->segment_table_full);return;
        }
        s=&a->segments[a->segments_used++];
        s->ctx=context;s->ordinal=++a->segment_ordinal;s->count=0;
    }
    if(s->count) segment_drop(a,s);
    s->writer=context;s->external=0;s->lo=s->hi=0;
    s->open=1;
}
void ps3dk_prof__auto_segment_close(struct _gcmCtxData *context)
{
    auto_capture *a;
    auto_segment *s;
    if(!context || !p_ready() || !(a=owner_capture(NULL,0)) || !(s=segment_find(a,context))) return;
    s->open=0;
}

/* cellGcmSegmentBeginExternal/EndExternal (observed build). The segment keeps its embedded context as its
 * identity (key), so cellGcmSegmentCall finds the build as for Begin/End; the writer is not known yet: the
 * application emits through a context of its own, bound at its first covered emitter (external_writer). A
 * new BeginExternal on the same segment drops a build that was never called, like Begin. */
void ps3dk_prof__auto_segment_external_open(struct _gcmCtxData *key,const void *buffer,uint32_t size)
{
    auto_capture *a;
    auto_segment *s;
    /* in_gpu is allowed here: an application's overflow callback may close one part and open the next in the
     * middle of an emitter that this profiler is bracketing (the bracket then ends as quarantined). */
    if(!key || !buffer || size<8u || !p_ready() || !(a=owner_capture(NULL,1))) return;
    if(!(s=segment_find(a,key))) {
        if(a->segments_used==AUTO_SEGMENTS_OPEN || a->segment_ordinal==UINT32_MAX) {
            increment(a,&a->segment_table_full);return;
        }
        s=&a->segments[a->segments_used++];
        s->ctx=key;s->ordinal=++a->segment_ordinal;s->count=0;
    }
    if(s->count) segment_drop(a,s);
    s->writer=NULL;s->external=1;
    s->lo=(uintptr_t)buffer;s->hi=(uintptr_t)buffer+size;
    s->open=1;
}
void ps3dk_prof__auto_segment_external_close(struct _gcmCtxData *key)
{
    auto_capture *a;
    auto_segment *s;
    if(!key || !p_ready() || !(a=owner_capture(NULL,0)) || !(s=segment_find(a,key))) return;
    s->open=0;s->writer=NULL;
}

/* The open external segment a context is writing: its current and end lie inside one claimed buffer. The
 * context becomes that segment's writer; an application that reuses one context for every part is bound to
 * whichever part is open where its cursor is. */
static auto_segment *external_writer(auto_capture *a,gpp_ctx *ctx)
{
    uintptr_t cur=(uintptr_t)(void *)ctx->current,end=(uintptr_t)(void *)ctx->end;
    for(uint32_t i=0;i<a->segments_used;i++) {
        auto_segment *s=&a->segments[i];
        if(!s->open || !s->external || cur<s->lo || cur>=s->hi || end<s->lo || end>=s->hi) continue;
        s->writer=ctx;
        return s;
    }
    return NULL;
}

/* A covered emitter on the owner thread writing into a context other than the
 * attached ring and not an open registered segment: a private/uninstrumented
 * buffer (or a segment the table could not register). It gets no markers and
 * its command runs unchanged, but it is never silently absent: the active
 * COVERAGE row counts it as an unsupported-form visit, or STATUS counts it as
 * unassociated when no frame is open. */
static void foreign_visit(auto_capture *a)
{
    increment(a,&a->foreign_visits);
    if(!auto_frames_unsupported(&a->coverage)) increment(a,&a->unassociated);
}

/* Two timestamp-report words, no label: the report alone, inside a segment. */
static void segment_report(gpp_ctx *ctx,uint32_t report)
{
    uint32_t *w=(uint32_t *)(void *)ctx->current;
    w[0]=(1u<<18)|0x1800u;              /* QUERY_GET, one word */
    w[1]=(1u<<24)|(report*16u);         /* timestamp report at that index */
    ctx->current=(void *)(w+2);
}

/* A covered emitter writing into an open registered segment: bracket it with
 * two reports in the segment (four words, current room only, no callback). The
 * operation-label release that completes it comes after the CALL, in ring
 * order. Returns the ticket (nonzero) or 0 after counting the exclusion. */
static uint32_t segment_begin(auto_capture *a,auto_segment *s,gpp_ctx *ctx,const ps3tc_auto_call *call,
                              uint64_t caller)
{
    const auto_wire_config *c=auto_config_read(&configuration);
    uint64_t words;
    auto_segment_op *b;
    int pair;
    uintptr_t cur=(uintptr_t)(void *)ctx->current,end=(uintptr_t)(void *)ctx->end;
    if(!auto_frames_count(&a->coverage,call->kind,0)) increment(a,&a->unassociated);
    increment(a,&a->segment_visits);
    if(!auto_config_can_admit(&configuration) || LD_ACQ(&g_prof->stop_latch)) {
        (void)auto_frames_count(&a->coverage,call->kind,LD_ACQ(&g_prof->stop_latch) ? 7:9);return 0;
    }
    if(a->pool_tail_column) {(void)auto_frames_count(&a->coverage,call->kind,a->pool_tail_column);return 0;}
    words=auto_command_words(call);
    if(!words) {(void)auto_frames_count(&a->coverage,call->kind,6);return 0;}
    if(!cur || (cur&3u) || cur>end || words>UINT32_MAX-4u || words+4u>(uint64_t)(end-cur)/4u) {
        (void)auto_frames_count(&a->coverage,call->kind,2);return 0;
    }
    if(a->segment_free==AUTO_SEGMENT_NONE) {
        a->pool_tail_column=4;(void)auto_frames_count(&a->coverage,call->kind,4);return 0;
    }
    if(!initialize_device(a)) {(void)auto_frames_count(&a->coverage,call->kind,10);return 0;}
    pair=auto_queue_take_pair(&a->operations);
    if(pair<0) {
        uint32_t column=a->operations.refusal==AUTO_QUEUE_STOPPED ? 9:3;
        if(column==3) a->pool_tail_column=3;
        (void)auto_frames_count(&a->coverage,call->kind,column);
        return 0;
    }
    a->segment_pending=pool_take(a);
    b=&a->segment_pool[a->segment_pending];memset(b,0,sizeof *b);b->pair=(uint32_t)pair;b->next=AUTO_SEGMENT_NONE;
    b->op.kind=(uint16_t)call->kind;b->op.caller=caller;b->op.generation=c->generation;
    b->op.flags=AUTO_OP_SEGMENT;b->op.segment=s->ordinal;
    b->op.args[0]=(uint32_t)words;
    if(call->kind==1) {b->op.args[1]=call->start;b->op.args[2]=call->count;b->op.args[5]=call->mode;}
    if(call->kind==2) {b->op.args[2]=call->count;b->op.args[3]=call->offset;
        b->op.args[4]=call->index_type;b->op.args[5]=call->mode;}
    if(call->kind==3) b->op.args[6]=call->clear_mask;
    b->op.report_begin=c->report_first+16+(uint32_t)pair*2;b->op.report_end=b->op.report_begin+1;
    a->operations_device.write_timer(a->operations_device.ctx,b->op.report_begin,PRG_POISON);
    a->operations_device.write_timer(a->operations_device.ctx,b->op.report_end,PRG_POISON);
    a->operations_device.sync(a->operations_device.ctx);
    b->op.tb_before=observation_tb();
    segment_report(ctx,b->op.report_begin);
    a->expected_end=(uintptr_t)(void *)ctx->current+words*4;
    a->segment_active=s;a->segment_active_writer=ctx;g_tls.in_gpu=1;
    return 1;
}

static void segment_end(auto_capture *a,gpp_ctx *ctx)
{
    auto_segment *s=a->segment_active;
    uint32_t slot=a->segment_pending;
    auto_segment_op *b=&a->segment_pool[slot];
    uintptr_t cur=(uintptr_t)(void *)ctx->current,end=(uintptr_t)(void *)ctx->end;
    a->segment_active=NULL;a->segment_active_writer=NULL;g_tls.in_gpu=0;a->segment_pending=AUTO_SEGMENT_NONE;
    if(cur!=a->expected_end || cur>end || end-cur<8) {
        /* The application did not advance as admitted (or a segment overflow
         * refused it): the begin report may run; no end report is written. */
        auto_queue_quarantine_pair(&a->operations,b->pair);
        ST_REL(&a->segment_quarantine,1u);
        (void)auto_frames_count(&a->coverage,b->op.kind,11);
        pool_give(a,slot);
        return;
    }
    segment_report(ctx,b->op.report_end);
    b->op.tb_after=observation_tb();
    if(b->op.tb_after<b->op.tb_before) b->op.flags|=8;
    (void)auto_frames_count(&a->coverage,b->op.kind,1);
    segment_append(a,s,slot);
}

/* cellGcmSegmentCall (observed build), after its CALL and retirement label.
 * One operation-label release in the ring, right here, completes every
 * operation of this build in execution order. Its records take the owner's
 * current frame. Anything else (no room, foreign ring, no metadata) commits the
 * build as INCOMPLETE records and quarantines its pairs. */
void ps3dk_prof__auto_segment_call(struct _gcmCtxData *ring,struct _gcmCtxData *segment)
{
    auto_capture *a;
    auto_segment *s;
    const auto_wire_config *c;
    gpp_ctx *ctx=(gpp_ctx *)(void *)ring;
    uint32_t share=0,call,i;
    uintptr_t cur,end;
    if(!ring || !segment || !p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,1))) return;
    if(!(s=segment_find(a,segment)) || !s->count) return;
    c=auto_config_read(&configuration);
    cur=(uintptr_t)(void *)ctx->current;end=(uintptr_t)(void *)ctx->end;
    if(ctx!=a->context || !c || c->reason || a->active || a->call_ordinal==UINT32_MAX ||
       LD_ACQ(&g_prof->stop_latch) || !auto_config_can_admit(&configuration) ||
       (cur&3u) || cur>end || end-cur<16 || a->operations.capacity-(uint32_t)(a->operations.write_idx-
       LD_ACQ(&a->operations.read_idx))<s->count) {
        segment_drop(a,s);return;
    }
    call=++a->call_ordinal;
    for(i=s->head;i!=AUTO_SEGMENT_NONE;i=a->segment_pool[i].next) {
        auto_segment_op *b=&a->segment_pool[i];
        auto_queue_entry *e=auto_queue_claim_held(&a->operations,b->pair,share);
        uint64_t id;
        uint32_t sb,se;
        if(!e) {auto_queue_quarantine_pair(&a->operations,b->pair);ST_REL(&a->segment_quarantine,1u);continue;}
        id=e->op.id;sb=e->op.serial_begin;se=e->op.serial_end;
        e->op=b->op;e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;e->op.call=call;
        if(a->coverage.active) {e->op.flags|=1;e->op.frame_owner=a->owner_incarnation;e->op.frame=a->coverage.live.frame;}
        ST_REL(&a->admitted,id);
        auto_queue_commit(&a->operations);
        share=se;
    }
    segment_release(a,s);
    if(share) {
        uint32_t *w=(uint32_t *)(void *)ctx->current;
        w[0]=(1u<<18)|0x1d6cu;w[1]=c->op_label*16u;   /* SEMAPHORE_OFFSET */
        w[2]=(1u<<18)|0x1d70u;                          /* BACKEND_WRITE_RELEASE */
        w[3]=(share&0xff00ff00u)|((share>>16)&0xffu)|((share&0xffu)<<16);
        ctx->current=(void *)(w+4);
        if(!auto_publication_commit(&a->publication,share)) auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
    }
}

/* D2: one FRAME mark (kind 4) in the ring at the owner's frame open (role 1) or
 * close (role 2): a report pair back to back, twelve words in current room, no
 * callback. Not coverage: a missed mark leaves that frame's RSX begin or end
 * UNKNOWN, visible in the reader. */
static void frame_mark(auto_capture *a,uint32_t role)
{
    const auto_wire_config *c=auto_config_read(&configuration);
    gpp_ctx *ctx=a->context;
    auto_queue_entry *e;
    uintptr_t cur=(uintptr_t)(void *)ctx->current,end=(uintptr_t)(void *)ctx->end;
    if(!a->coverage.active || a->active || !c || c->reason || LD_ACQ(&g_prof->stop_latch) ||
       !auto_config_can_admit(&configuration)) return;
    if((uintptr_t)(void *)ctx->begin!=clean_begin || end!=clean_end || (cur&3u) || cur>end || end-cur<48 ||
       !initialize_device(a) || !(e=auto_queue_claim(&a->operations))) {
        increment(a,&a->frame_marks_missed);return;
    }
    ST_REL(&a->admitted,e->op.id);
    e->op.kind=AUTO_KIND_FRAME;e->op.flags=1;e->op.generation=c->generation;e->op.args[0]=role;
    e->op.frame_owner=a->owner_incarnation;e->op.frame=a->coverage.live.frame;
    e->op.report_begin=c->report_first+16+e->pair*2;e->op.report_end=e->op.report_begin+1;
    a->operations_device.write_timer(a->operations_device.ctx,e->op.report_begin,PRG_POISON);
    a->operations_device.write_timer(a->operations_device.ctx,e->op.report_end,PRG_POISON);
    a->operations_device.sync(a->operations_device.ctx);
    g_tls.in_gpu=1;
    e->op.tb_before=observation_tb();
    if(a->operations_device.reserve_pair(a->operations_device.ctx,e->op.report_begin,c->op_label,e->op.serial_begin)!=0 ||
       a->operations_device.reserve_pair(a->operations_device.ctx,e->op.report_end,c->op_label,e->op.serial_end)!=0) {
        e->op.flags|=4|32;
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);
    }
    e->op.tb_after=observation_tb();
    if(e->op.tb_after<e->op.tb_before) e->op.flags|=8;
    {
        int complete=!(e->op.flags&4);
        uint32_t serial=e->op.serial_end;
        auto_queue_commit(&a->operations);
        if(complete && !auto_publication_commit(&a->publication,serial))
            auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
    }
    g_tls.in_gpu=0;
}

/* A covered emitter on a context other than the attached ring. */
static uint32_t foreign_begin(gpp_ctx *ctx,const ps3tc_auto_call *call,uint64_t caller)
{
    auto_capture *a;
    auto_segment *s;
    if(!call || call->kind<1 || call->kind>3 || g_tls.in_gpu || !(a=owner_capture(NULL,1)) || a->active ||
       a->segment_active) return 0;
    s=segment_find(a,ctx);
    if(s && s->open && !s->external) return segment_begin(a,s,ctx,call,caller);
    if((s=external_writer(a,ctx))) return segment_begin(a,s,ctx,call,caller);
    foreign_visit(a);
    return 0;
}

uint32_t ps3dk_prof__auto_begin(struct _gcmCtxData *context,const ps3tc_auto_call *call,uint64_t caller)
{
    gpp_ctx *ctx=(gpp_ctx *)(void *)context;
    if(!ctx || !p_ready()) return 0;
    auto_capture *a=owner_capture(ctx,1);
    const auto_wire_config *c;
    auto_queue_entry *e;
    uint64_t words;
    if(!a) return foreign_begin(ctx,call,caller);
    if(!call || call->kind<1 || call->kind>3) return 0;
    if(g_tls.in_gpu && !a->active) {increment(a,&a->internal_suppressed);return 0;}
    if(!auto_frames_count(&a->coverage,call->kind,0)) increment(a,&a->unassociated);
    if(a->active) { (void)auto_frames_count(&a->coverage,call->kind,5);return 0; }
    if(LD_ACQ(&g_prof->stop_latch) || !auto_config_can_admit(&configuration)) {
        /* An application using only the selected GCM wrapper still needs an
         * owner stop acknowledgement. No new marker is admitted here. */
        int stopped=LD_ACQ(&g_prof->stop_latch)!=0;
        (void)auto_frames_count(&a->coverage,call->kind,stopped ? 7:9);
        if(stopped) p_observe_stop(g_prof);
        return 0;
    }
    if((uintptr_t)(void *)ctx->begin!=clean_begin || (uintptr_t)(void *)ctx->end!=clean_end) {
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);
        (void)auto_frames_count(&a->coverage,call->kind,11);
        return 0;
    }
    if(a->pool_tail_column) { (void)auto_frames_count(&a->coverage,call->kind,a->pool_tail_column);return 0; }
    words=auto_command_words(call);
    if(!words) { (void)auto_frames_count(&a->coverage,call->kind,6);return 0; }
    if(!auto_command_room((uintptr_t)(void *)ctx->begin,(uintptr_t)(void *)ctx->current,
                         (uintptr_t)(void *)ctx->end,words)) {
        (void)auto_frames_count(&a->coverage,call->kind,2);return 0;
    }
    c=auto_config_read(&configuration);
    if(!initialize_device(a)) {
        (void)auto_frames_count(&a->coverage,call->kind,10);return 0;
    }
    e=auto_queue_claim(&a->operations);
    if(!e) {
        uint32_t column=a->operations.refusal==AUTO_QUEUE_METADATA ? 4:
                        a->operations.refusal==AUTO_QUEUE_REPORTS ? 3:
                        a->operations.refusal==AUTO_QUEUE_REENTRY ? 5:
                        a->operations.refusal==AUTO_QUEUE_STOPPED ? 9:11;
        if(column==3 || column==4) a->pool_tail_column=column;
        (void)auto_frames_count(&a->coverage,call->kind,column);return 0;
    }
    ST_REL(&a->admitted,e->op.id);
    e->op.kind=(uint16_t)call->kind;e->op.caller=caller;e->op.generation=c->generation;
    if(a->coverage.active) {e->op.flags=1;e->op.frame_owner=a->owner_incarnation;e->op.frame=a->coverage.live.frame;}
    e->op.args[0]=(uint32_t)words;
    if(call->kind==1) {e->op.args[1]=call->start;e->op.args[2]=call->count;e->op.args[5]=call->mode;}
    if(call->kind==2) {e->op.args[2]=call->count;e->op.args[3]=call->offset;
        e->op.args[4]=call->index_type;e->op.args[5]=call->mode;}
    if(call->kind==3) e->op.args[6]=call->clear_mask;
    e->op.report_begin=c->report_first+16+e->pair*2;e->op.report_end=e->op.report_begin+1;
    a->operations_device.write_timer(a->operations_device.ctx,e->op.report_begin,PRG_POISON);
    a->operations_device.write_timer(a->operations_device.ctx,e->op.report_end,PRG_POISON);
    a->operations_device.sync(a->operations_device.ctx);
    a->active=e;g_tls.in_gpu=1;
    e->op.tb_before=observation_tb();
    a->expected_end=(uintptr_t)(void *)ctx->current+(words+6)*4;
    /* The complete app+markers room check above excludes callbacks. The end
     * verifies the actual application's advancement before issuing its marker. */
    if(a->operations_device.reserve_pair(a->operations_device.ctx,e->op.report_begin,c->op_label,e->op.serial_begin)!=0) {
        e->op.flags|=4|32;e->op.tb_after=observation_tb();
        auto_queue_commit(&a->operations);a->active=NULL;g_tls.in_gpu=0;
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);
        (void)auto_frames_count(&a->coverage,call->kind,11);
        return 0;
    }
    return e->op.serial_end;
}

void ps3dk_prof__auto_end(struct _gcmCtxData *context,uint32_t ticket)
{
    auto_capture *a;
    auto_queue_entry *e;
    const auto_wire_config *c;
    if(!ticket) return;
    a=owner_capture((gpp_ctx *)(void *)context,0);
    if(!a) {
        a=owner_capture(NULL,0);
        if(a && a->segment_active && a->segment_active_writer==(void *)context) segment_end(a,(gpp_ctx *)(void *)context);
        return;
    }
    if(!(e=a->active)) return;
    c=auto_config_read(&configuration);
    uintptr_t current=(uintptr_t)(void *)a->context->current,end=(uintptr_t)(void *)a->context->end;
    if(ticket!=e->op.serial_end || current!=a->expected_end || end<current || end-current<24 ||
       a->operations_device.reserve_pair(a->operations_device.ctx,e->op.report_end,c->op_label,e->op.serial_end)!=0) {
        e->op.flags|=4|32;
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);
    }
    e->op.tb_after=observation_tb();
    if(e->op.tb_after<e->op.tb_before) e->op.flags|=8;
    uint32_t serial=e->op.serial_end;
    int complete=!(e->op.flags&4);
    (void)auto_frames_count(&a->coverage,e->op.kind,complete ? 1:11);
    auto_queue_commit(&a->operations);
    if(complete && !auto_publication_commit(&a->publication,serial))
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
    a->active=NULL;g_tls.in_gpu=0;
    if(LD_ACQ(&g_prof->stop_latch)) p_observe_stop(g_prof);
}

uint32_t ps3dk_prof__auto_publish_before(struct _gcmCtxData *context)
{
    if(!context) return 0;
    auto_capture *a=owner_capture((gpp_ctx *)(void *)context,0);
    if(a) a->clock_put_frontier=auto_clock_before(&a->clocks);
    return a ? auto_publication_before(&a->publication):0;
}
void ps3dk_prof__auto_publish_after(struct _gcmCtxData *context,uint32_t frontier)
{
    if(!context) return;
    auto_capture *a=owner_capture((gpp_ctx *)(void *)context,0);
    if(a && !auto_publication_after(&a->publication,frontier))
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
    if(a && !auto_clock_after(&a->clocks,a->clock_put_frontier))
        auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
}
void ps3dk_prof__auto_context_reset(struct _gcmCtxData *context)
{
    if(!context || !p_ready()) return;
    auto_capture *a=owner_capture((gpp_ctx *)(void *)context,1);
    if(a) {auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);}
}

static void present_boundary(auto_capture *a);
void p_auto_owner_retire(void)
{
    auto_capture *a=owner_capture(NULL,1);
    if(a && !a->active) {
        if(a->fifo_instance) (void)ps3dk_prof_fifo_detach(a->fifo_instance);
        for(uint32_t i=0;i<a->segments_used;i++) if(a->segments[i].count) segment_drop(a,&a->segments[i]);
        if(a->coverage.active) {
            present_boundary(a);
            frame_mark(a,AUTO_FRAME_END);
            (void)auto_frames_close(&a->coverage,a->owner_incarnation,a->coverage.live.frame,observation_tb(),1);
        }
        ST_REL(&a->owner_done,1u);
    }
}

void p_auto_frame(uint64_t frame,uint64_t tb)
{
    auto_capture *a=owner_capture(NULL,1);
    if(a && !a->active && !g_tls.in_gpu && auto_config_can_admit(&configuration)) {
        int flip=a->in_flip_frame!=0;
        if(!flip) a->explicit_seen=1; /* an explicit call takes precedence from here on */
        present_boundary(a);
        if(a->coverage.active) frame_mark(a,AUTO_FRAME_END); /* implied closure of the previous frame */
        if(frame%8u==0) sample_clock(a,frame);
        if(LD_ACQ(&g_prof->stop_latch)) {p_observe_stop(g_prof);return;}
        a->pool_tail_column=0; /* each frame starts admitting again; its own exhaustion makes its own tail */
        if(auto_frames_open(&a->coverage,frame,tb)) {
            a->coverage.live.flags|=AUTO_FRAME_ROW_PRESENT; /* this writer observes presentation (block at 352) */
            if(flip) {a->coverage.live.flags|=AUTO_FRAME_ROW_FLIP;increment(a,&a->frames_by_flip);}
            else increment(a,&a->frames_by_explicit);
            frame_mark(a,AUTO_FRAME_BEGIN);
        }
    }
}

/* Explicit closure of the owner's active frame: its end mark, the row, and the
 * 64-frame block-end clock anchor. 0 when the token is not the active frame. */
static int close_frame(auto_capture *a,uint32_t owner,uint64_t frame)
{
    if(a->coverage.active && owner==a->coverage.owner && frame==a->coverage.live.frame) {
        present_boundary(a);
        frame_mark(a,AUTO_FRAME_END);
    }
    if(!auto_frames_close(&a->coverage,owner,frame,observation_tb(),0)) return 0;
    if(frame%64u==63) sample_clock(a,frame);
    return 1;
}

/* Presentation observation on the owner thread. No device access; no lock.
 *
 * Polling cadence (gcmGetFlipStatus in the selected build): a run is a
 * sequence of consecutive polls that saw the flip pending, closed by a poll
 * that saw it done. Its evidence is the sum of the gaps between consecutive
 * polls (previous return to next entry), counted only while every gap is at
 * most cadence_bound (1 ms). A larger gap ends the run as broken; the next
 * pending poll starts a new one. Cadence bounds how often the owner looked at
 * the flip, not whether it was waiting: the owner may have done useful work
 * between any two polls, so cadence never establishes presentation pacing.
 * 1 ms is below a sixteenth of the shortest display period (16.68 ms at
 * 59.94 Hz), far below any frame the verdict could judge, and above the cost
 * of a poll and a short sleep, so a loop that only polls and sleeps briefly
 * stays inside it while a loop doing visible work between polls does not.
 *
 * Flip-wait scope (cellGcmWaitFlipComplete): the helper's semantics are
 * waiting for flip completion, so its outer elapsed time is waiting
 * evidence: flip-wait scope elapsed time, including scheduling and polling,
 * never exact RSX wait or device completion time. Polls inside a scope are
 * part of it and are not counted again as cadence. */
static void add32(uint32_t *v) {if(*v!=UINT32_MAX) ++*v;}
static void add64(uint64_t *v,uint64_t n) {*v=UINT64_MAX-*v<n ? UINT64_MAX:*v+n;}
static void cadence_close(auto_capture *a,int complete)
{
    auto_wire_frame *f=&a->coverage.live;
    a->cadence_open=0;
    if(!a->coverage.active) return;
    if(a->cadence_spanning) add32(&f->cadence_spanning); /* charged to neither frame */
    else if(!complete) add32(&f->cadence_broken);
    else {add32(&f->cadence_runs);add64(&f->cadence,a->cadence_ticks);}
}
/* The gap from the previous pending poll's return to this poll's entry: 1 and
 * counted when within the bound, 0 when it breaks the run. */
static int cadence_gap(auto_capture *a,uint64_t before)
{
    uint64_t gap;
    if(before<a->cadence_last || (gap=before-a->cadence_last)>a->cadence_bound) return 0;
    add64(&a->cadence_ticks,gap);
    return 1;
}
/* A frame boundary (a frame opens, closes or is closed by its successor)
 * while a cadence run or a flip-wait scope is open: the interval has no single
 * owning frame. Each row it touches counts it as spanning, now for the row
 * being closed and at its end for the row then active; no row is charged and
 * nothing is clipped. Call before the row closes. */
static void present_boundary(auto_capture *a)
{
    auto_wire_frame *f=&a->coverage.live;
    if(a->cadence_open) {
        a->cadence_spanning=1;
        if(a->coverage.active) add32(&f->cadence_spanning);
    }
    if(a->flip_wait_open) {
        a->flip_wait_spanning=1;
        if(a->coverage.active) add32(&f->flip_wait_spanning);
    }
}
uint64_t ps3dk_prof__auto_flip_status_begin(void)
{
    auto_capture *a;
    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return 0;
    return observation_tb()|1u; /* never 0 for an observed call */
}
void ps3dk_prof__auto_flip_status_end(uint64_t before,uint32_t status)
{
    auto_capture *a;
    if(!before || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return;
    if(status) {
        if(a->cadence_open && !cadence_gap(a,before)) cadence_close(a,0);
        if(!a->cadence_open) {a->cadence_open=1;a->cadence_spanning=0;a->cadence_ticks=0;}
        a->cadence_last=observation_tb();
        return;
    }
    if(a->cadence_open) cadence_close(a,cadence_gap(a,before));
}

uint64_t ps3dk_prof__auto_flip_wait_begin(void)
{
    auto_capture *a;
    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return 0;
    if(a->cadence_open) cadence_close(a,0); /* the scope takes over: the run never saw its done poll */
    a->flip_wait_open=1;a->flip_wait_spanning=0;a->flip_wait_since=observation_tb();
    return a->flip_wait_since|1u;
}
void ps3dk_prof__auto_flip_wait_end(uint64_t scope,int32_t rc)
{
    auto_capture *a;
    uint64_t after;
    if(!scope || !(a=owner_capture(NULL,0)) || !a->flip_wait_open) return;
    after=observation_tb();
    a->flip_wait_open=0;
    if(!a->coverage.active) return;
    if(a->flip_wait_spanning) {add32(&a->coverage.live.flip_wait_spanning);return;} /* charged to neither */
    if(after<a->flip_wait_since) return;
    add32(&a->coverage.live.flip_waits);add64(&a->coverage.live.flip_wait,after-a->flip_wait_since);
    if(rc) add32(&a->coverage.live.flip_wait_timeouts);
}

/* A flip queued on the owner thread through the GCM layer: it closes the
 * active frame and opens the next one through the core frame call, so FRAME
 * events, clock anchors and coverage behave as for explicit frames. Before the
 * first flip nothing is associated; after an explicit ps3dk_prof_frame, flips
 * no longer delimit frames and are counted as ignored. */
void ps3dk_prof__auto_flip(struct _gcmCtxData *context,int32_t ok)
{
    auto_capture *a;
    if(!ok || !context || !p_ready() || g_tls.in_gpu) return;
    a=owner_capture((gpp_ctx *)(void *)context,1);
    if(!a || a->active || a->segment_active || !auto_config_can_admit(&configuration)) return;
    if(a->explicit_seen) {increment(a,&a->flips_ignored);return;}
    if(a->coverage.active) (void)close_frame(a,a->coverage.owner,a->coverage.live.frame);
    a->in_flip_frame=1;
    ps3dk_prof_frame();
    a->in_flip_frame=0;
}

int ps3dk_prof_auto_frame_end(uint32_t instance,ps3dk_prof_token frame)
{
    if(!p_ready()) return PS3DK_PROF_ENOTINIT;
    if(instance!=1 || !frame.owner || frame.reserved) return PS3DK_PROF_EINVAL;
    auto_capture *a=owner_capture(NULL,1);
    if(!a || a->active || g_tls.in_gpu) return PS3DK_PROF_EINVAL;
    if(!close_frame(a,frame.owner,frame.frame)) return PS3DK_PROF_EINVAL;
    if(LD_ACQ(&g_prof->stop_latch)) p_observe_stop(g_prof);
    return PS3DK_PROF_OK;
}

#ifdef PS3DK_PROF_TEST_HOOKS
/* Host controls only: clock records published by the owner and not yet
 * accepted by the drainer. Not part of any installed archive. */
uint64_t ps3dk_prof__auto_test_clock_backlog(void)
{
    const auto_capture *a=capture;
    return a ? LD_ACQ(&a->clocks.write_idx)-LD_ACQ(&a->clocks.read_idx):0;
}
/* Owner thread only: the active row's presentation fields: flip-wait scopes,
 * their ticks, cadence runs, cadence ticks, broken runs, spanning runs,
 * spanning scopes, timed-out scopes. */
void ps3dk_prof__auto_test_present(uint64_t out[8])
{
    const auto_capture *a=capture;
    const auto_wire_frame *f=a && a->coverage.active ? &a->coverage.live:NULL;
    out[0]=f ? f->flip_waits:0;out[1]=f ? f->flip_wait:0;out[2]=f ? f->cadence_runs:0;out[3]=f ? f->cadence:0;
    out[4]=f ? f->cadence_broken:0;out[5]=f ? f->cadence_spanning:0;out[6]=f ? f->flip_wait_spanning:0;
    out[7]=f ? f->flip_wait_timeouts:0;
}
/* Owner thread only: frames by source and flips ignored after an explicit call. */
void ps3dk_prof__auto_test_frames(uint64_t out[3])
{
    const auto_capture *a=capture;
    out[0]=a ? a->frames_by_flip:0;out[1]=a ? a->frames_by_explicit:0;out[2]=a ? a->flips_ignored:0;
}
/* Owner thread only: internal split of the unsupported-form visits. */
void ps3dk_prof__auto_test_segment_counts(uint64_t out[4])
{
    const auto_capture *a=capture;
    out[0]=a ? a->segment_visits:0;out[1]=a ? a->foreign_visits:0;
    out[2]=a ? a->segment_table_full:0;out[3]=a ? a->segments_used:0;
}
#endif

static uint32_t completion_label(void *user)
{
    auto_capture *a=user;
    return a->operations_device.read_label(a->operations_device.ctx,a->operations_port.label);
}
static uint64_t completion_timer(void *user,uint32_t slot)
{
    auto_capture *a=user;
    return a->operations_device.read_timer(a->operations_device.ctx,slot);
}
static void completion_sync(void *user)
{
    auto_capture *a=user;
    a->operations_device.sync(a->operations_device.ctx);
}

/* Drainer only: one OPS chunk carries up to this many records; each is
 * acknowledged, in order, after the whole chunk was accepted. */
enum { AUTO_OPS_BATCH=64 };
static auto_wire_op batch[AUTO_OPS_BATCH];
static uint8_t batch_ready[AUTO_OPS_BATCH];

static int drain_operations(auto_capture *a,int seal)
{
    const auto_queue_entry *head;
    auto_completion_port port={a,completion_label,completion_timer,completion_sync};
    for(;;) {
    uint32_t n=0,i;
    int stop=0;
    while(n<AUTO_OPS_BATCH && (head=auto_queue_peek_at(&a->operations,n))!=NULL) {
        auto_wire_op record=head->op;
        auto_completion_fact fact={0};
        int result=AUTO_COMPLETION_REFUSED;
        if(!(record.flags&4) && !auto_config_flags(&configuration))
            result=auto_completion_read(&a->completion,&a->publication,&port,
                                        record.report_begin,record.report_end,
                                        record.serial_begin,record.serial_end,&fact);
        if((result==AUTO_COMPLETION_PENDING || result==AUTO_COMPLETION_DEFER) && !seal) {stop=1;break;}
        if(result==AUTO_COMPLETION_READY) {
            record.flags|=2;record.raw_begin=fact.raw_begin;record.raw_end=fact.raw_end;
            record.published=fact.published;
        } else {
            record.flags|=4;
            record.raw_begin=record.raw_end=0;
            record.published=auto_publication_read(&a->publication);
            if(record.published<record.serial_end) record.flags|=32;
            if(record.flags&32) record.published=0; /* PUBLICATION_UNKNOWN carries no snapshot */
            if(result==AUTO_COMPLETION_REFUSED) record.flags|=16;
        }
        if(a->completion.invalid) {
            auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);
        }
        batch[n]=record;batch_ready[n]=result==AUTO_COMPLETION_READY;n++;
    }
    if(!n) return 1;
    if(!auto_writer_ops(&writer,batch,n)) return 0;
    for(i=0;i<n;i++)
        if(!auto_queue_ack(&a->operations,batch[i].id,batch_ready[i],1)) return 0;
    if(stop) return 1;
    }
}

static int chunk(void *user, uint32_t type, const uint8_t *data, size_t bytes)
{
    return p_fifo_emit_chunk((prof *)user,type,data,bytes);
}

void p_auto_init(prof *p)
{
    auto_config_init(&configuration,p->tb_freq,ps3dk_prof__auto_mechanism,ps3dk_prof__auto_build_binding);
    (void)auto_writer_init(&writer,&configuration,p_fifo_payload(p),P_CHUNK_CAP,chunk,p);
}

void p_auto_seal_claim(void)
{
    auto_config_stop(&configuration);
}

void p_auto_drain(prof *p, int seal)
{
    const auto_wire_config *c;
    uint8_t b[64]={0};
    uint64_t totals[15]={0};
    if(p->snk.failed) return;
    if(seal) auto_config_stop(&configuration);
    c=auto_config_read(&configuration);
    if(!c) {
        /* A setup owner still holds WRITING after the bounded quiesce wait.
         * Neither its candidate nor an invented refusal is a frozen fact.
         * Do not certify a terminal capture with missing AUTO configuration. */
        if(seal) p_ext_sink_fail(p,-2);
        return;
    }
    if(c->reason) { (void)auto_writer_begin(&writer);return; }
    if(!capture) {p_ext_sink_fail(p,-2);return;}
    /* THREADS acceptance is a capture fact. The core clears decl_gen when it
     * retires/reuses a slot; that cannot revoke an already-declared owner. */
    if(!capture->owner_declared && p->dr.decl_gen[capture->owner_slot]==capture->owner_generation)
        capture->owner_declared=1;
    if(!capture->owner_declared) {
        if(seal) p_ext_sink_fail(p,-2);
        return;
    }
    if(!capture->domain_declared) {
        p_le32(b,1);p_le32(b+4,c->domain);b[8]=2;b[9]=64;b[10]=2;p_le32(b+12,2);
        p_le16(b+52,3);memcpy(b+54,"rsx",3);
        if(chunk(p,8,b,57)!=0) return;
        capture->domain_declared=1;
    }
    if(!capture->epoch_declared) {
        memset(b,0,sizeof b);p_le32(b,1);p_le32(b+4,c->domain);p_le32(b+8,1);b[12]=1;p_le64(b+24,p_tb());
        if(chunk(p,9,b,32)!=0) return;
        capture->epoch_declared=1;
        p->gpu_declared|=UINT64_C(1)<<(c->domain-GPU_ID_BASE);
    }
    if(!auto_writer_begin(&writer)) return;
    if(!drain_operations(capture,seal)) return;
    const auto_wire_frame *frame;
    while((frame=auto_frames_peek(&capture->coverage))!=NULL) {
        uint32_t owner=frame->owner;uint64_t id=frame->frame;
        if(!auto_writer_frame(&writer,frame)) return;
        if(!auto_frames_ack(&capture->coverage,owner,id,1)) {p_ext_sink_fail(p,-2);return;}
    }
    const auto_wire_clock *clock;
    while((clock=auto_clock_peek(&capture->clocks))!=NULL) {
        uint32_t id=clock->id;
        if(!auto_writer_clock(&writer,clock)) return;
        if(!auto_clock_ack(&capture->clocks,id,1)) {p_ext_sink_fail(p,-2);return;}
    }
    if(seal && !capture->final_declared) {
        uint16_t flags=AUTO_FINAL;
        uint64_t admitted=LD_ACQ(&capture->admitted);
        totals[0]=totals[1]=admitted;totals[2]=writer.operations_written;
        totals[4]=admitted-writer.operations_written;
        totals[5]=writer.frames_written;totals[6]=LD_ACQ(&capture->coverage.lost);
        totals[7]=LD_ACQ(&capture->unassociated);totals[8]=LD_ACQ(&capture->internal_suppressed);
        totals[9]=LD_ACQ(&capture->clocks.attempted);totals[10]=writer.clocks_written;
        totals[11]=LD_ACQ(&capture->clocks.lost);
        totals[12]=LD_ACQ(&capture->conflicts);totals[13]=auto_publication_read(&capture->publication);
        totals[14]=auto_queue_held(&capture->operations);
        if(totals[4] || totals[6] || totals[14]) flags|=AUTO_LOWER_BOUND;
        if(totals[9]<totals[10] || totals[9]-totals[10]!=totals[11]) flags|=AUTO_LOWER_BOUND;
        if(!LD_ACQ(&capture->owner_done)) flags|=AUTO_LOWER_BOUND|AUTO_OWNER_UNACK;
        if(LD_ACQ(&capture->segment_quarantine)) flags|=AUTO_STATUS_SEGMENT_QUARANTINE;
        if(LD_ACQ(&capture->frame_marks_missed)) flags|=AUTO_STATUS_FRAME_MARK_MISSED;
        if(LD_ACQ(&capture->frames_by_flip)) flags|=AUTO_STATUS_FRAMES_FLIP;
        if(LD_ACQ(&capture->frames_by_explicit)) flags|=AUTO_STATUS_FRAMES_EXPLICIT;
        if(LD_ACQ(&capture->saturated) || LD_ACQ(&capture->coverage.saturated) ||
           LD_ACQ(&capture->clocks.saturated)) flags|=AUTO_LOWER_BOUND|AUTO_SATURATED;
        if(!auto_writer_status(&writer,flags,totals)) {p_ext_sink_fail(p,-2);return;}
        memset(b,0,sizeof b);p_le32(b,1);p_le32(b+4,c->domain);b[8]=2;b[9]=1;
        if(chunk(p,16,b,40)!=0) return;
        capture->final_declared=1;
    }
}
