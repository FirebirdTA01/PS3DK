#include "auto_internal.h"
#include "auto_commands.h"
#include "../fifo/fifo_internal.h"
#include "../rsx/gpu_internal.h"
#include <string.h>
#include <stddef.h>

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

/* The two reserves for a pool of `pairs`: whole-CALL brackets (32, a quarter of a small pool) and AUTO ranges (the
 * option, or 16 / an eighth of a small pool). 0 when an explicit range reserve leaves fewer than 64 pairs. */
static uint32_t call_pairs(uint32_t pairs) {return pairs>=4u*AUTO_CALL_PAIRS ? AUTO_CALL_PAIRS:pairs/4u;}
/* The published option sizes: 32 (no range_pairs), 36 (no ring fields) and the current 52. */
enum { AUTO_OPTIONS_V32=offsetof(ps3dk_prof_auto_options,range_pairs),
       AUTO_OPTIONS_V36=offsetof(ps3dk_prof_auto_options,ring_blocks) };
_Static_assert(AUTO_OPTIONS_V32==32 && AUTO_OPTIONS_V36==36 && sizeof(ps3dk_prof_auto_options)==52,
               "published ps3dk_prof_auto_options sizes");
/* Copies exactly the caller's published size, zero-extending the rest; size keeps the caller's value. Nothing
 * past that size is ever read from the caller. */
static int options_copy(const ps3dk_prof_auto_options *o, ps3dk_prof_auto_options *n)
{
    memset(n,0,sizeof *n);
    if(o->size!=sizeof *n && o->size!=AUTO_OPTIONS_V36 && o->size!=AUTO_OPTIONS_V32) return 0;
    memcpy(n,o,o->size);
    return 1;
}
static int ring_requested(const ps3dk_prof_auto_options *o)
{
    return o->ring_blocks || o->ring_label || o->ring_bytes || o->ring_records;
}
/* The local pool the legacy pair allocator owns: all of it, or what the ring blocks leave. */
static uint32_t legacy_pool(const ps3dk_prof_auto_options *o, const auto_ring_plan *ring)
{
    return ring->blocks ? ring->legacy_pairs:(o->report_count-16)/2;
}
static int range_pairs(const ps3dk_prof_auto_options *o,const auto_ring_plan *ring,uint32_t *out)
{
    uint32_t pairs=legacy_pool(o,ring),call=call_pairs(pairs);
    if(o->size<AUTO_OPTIONS_V36 || !o->range_pairs) {*out=pairs>=8u*AUTO_RANGE_PAIRS ? AUTO_RANGE_PAIRS:pairs/8u;return 1;}
    if(pairs<call+64u || o->range_pairs>pairs-call-64u) return 0;
    *out=o->range_pairs;
    return 1;
}
/* ring is zeroed unless the (normalized) options select the report ring. */
static int ring_options(const ps3dk_prof_auto_options *o, auto_ring_plan *ring, uint32_t *label)
{
    memset(ring,0,sizeof *ring);*label=0;
    if(!ring_requested(o)) return 1;
#ifdef PS3DK_PROF_AUTO_RING
    *label=o->ring_label ? o->ring_label:AUTO_RING_DEFAULT_LABEL;
    if(!label_valid(*label) || *label==o->operation_label || *label==o->clock_label) return 0;
    return auto_ring_layout(o->report_first,o->report_count,o->ring_blocks,o->ring_bytes,o->ring_records,
                            sizeof(auto_queue_entry),sizeof(auto_ring_slot),ring);
#else
    return 0; /* reserved until the ring writer, drainer and reader are integrated */
#endif
}
static int layout(const ps3dk_prof_auto_options *o, auto_ring_plan *ring, uint32_t *ring_label,
                  size_t *records, size_t *frames, size_t *total)
{
    uint64_t n;
    uint32_t ranges;
    if(o->report_count<18 || o->report_count>1024 || (o->report_count&1) ||
       o->report_first>2048-o->report_count || !label_valid(o->operation_label) ||
       !label_valid(o->clock_label) || o->operation_label==o->clock_label ||
       !o->metadata_records || o->metadata_records>1024 || !o->frame_records || o->frame_records>256 ||
       !o->max_bytes || o->max_bytes>2097152 || !ring_options(o,ring,ring_label) || !range_pairs(o,ring,&ranges)) return 0;
    /* Every multiplier is bounded above before these widened expressions.
     * Include padding in the published allocation, not just symbol payloads.
     * Ring metadata and descriptors live in the separate page allocation. */
    n=((uint64_t)sizeof(auto_capture)+127u)&~UINT64_C(127);
    *records=(size_t)n;
    if(!ring->blocks) n+=(uint64_t)o->metadata_records*sizeof(auto_queue_entry);
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

static void queue_reserves(auto_capture *a, const ps3dk_prof_auto_options *o, const auto_ring_plan *ring)
{
    uint32_t ranges=0;
    (void)range_pairs(o,ring,&ranges);   /* validated by layout */
    auto_queue_set_reserve(&a->operations,AUTO_PAIR_CALL,call_pairs(legacy_pool(o,ring)));
    auto_queue_set_reserve(&a->operations,AUTO_PAIR_RANGE,ranges);
}

#ifdef PS3DK_PROF_AUTO_RING
/* Process lifetime: the one reusable MAIN page allocation (mapped; a capture that never issued device commands
 * leaves it unreferenced), and the bytes of allocations that can never be reused or freed. Only the configuring
 * owner, under CONFIG's WRITING claim, touches these. */
static struct { uintptr_t ea; uint32_t total, bytes, offset; } ring_memory;
static uint64_t ring_retained;

#ifdef PS3DK_PROF_PAL_LV2
#include <sys/memory.h>
static int ring_allocate_lv2(uint32_t bytes, uintptr_t *ea)
{
    sys_addr_t addr=0;
    if(sys_memory_allocate(bytes,SYS_MEMORY_PAGE_SIZE_1M,&addr)!=0 || !addr) return -1;
    *ea=(uintptr_t)addr;
    return 0;
}
static int ring_map_lv2(uintptr_t ea, uint32_t bytes, uint32_t *offset)
{
    return cellGcmMapMainMemory((const void *)ea,bytes,offset)==0 ? 0:-1;
}
static const auto_ring_memory gpp_ring_memory_gcm={ring_allocate_lv2,ring_map_lv2};
#endif

static const auto_ring_memory *ring_memory_ops(void)
{
#ifdef PS3DK_PROF_PAL_LV2
    return &gpp_ring_memory_gcm;
#elif defined(PS3DK_PROF_TEST_HOOKS)
    return (const auto_ring_memory *)ps3dk_prof_test_hooks.auto_ring_memory;
#else
    return NULL;
#endif
}

/* 0, or the CONFIG refusal reason. Allocates (or reuses) and maps before any admission; the metadata queue and the
 * descriptors are initialized in the allocation's unmapped tail. */
static uint32_t ring_attach(auto_capture *a, const ps3dk_prof_auto_options *o, const auto_ring_plan *plan)
{
    const auto_ring_memory *m=ring_memory_ops();
    uintptr_t ea=0;
    uint32_t offset=0;
    if(ring_memory.ea && ring_memory.total==plan->total && ring_memory.bytes==plan->bytes) {
        ea=ring_memory.ea;offset=ring_memory.offset;
    } else {
        if(!m || !m->allocate || !m->map || m->allocate(plan->total,&ea)!=0 || !ea) return AUTO_ALLOCATION_FAILED;
        /* Kept from here on: a failed map may still have made the pages GPU visible. */
        if((ea&(AUTO_RING_PAGE-1u)) || m->map(ea,plan->bytes,&offset)!=0 || (offset&15u) ||
           (uint64_t)offset+plan->bytes>UINT64_C(0x100000000)) {
            ring_retained+=plan->total;return AUTO_ALLOCATION_FAILED;
        }
        if(ring_memory.ea) ring_retained+=ring_memory.total;
        ring_memory.ea=ea;ring_memory.total=plan->total;ring_memory.bytes=plan->bytes;ring_memory.offset=offset;
    }
    memset((void *)(ea+plan->metadata_at),0,plan->total-plan->metadata_at);
    if(!auto_ring_init(&a->ring,(auto_ring_slot *)(void *)(ea+plan->slots_at),plan->slots,plan->blocks,plan->pairs) ||
       !auto_queue_init_wide(&a->operations,(auto_queue_entry *)(void *)(ea+plan->metadata_at),plan->records,
                             plan->legacy_pairs)) return AUTO_UNSUPPORTED_CONTEXT;
    queue_reserves(a,o,plan);
    a->ring_plan=*plan;a->ring_ea=ea;a->ring_offset=offset;
    return 0;
}
#endif

static int attach(ps3dk_prof_gcm_ctx *ctx, const ps3dk_prof_auto_options *options,
                  uint32_t *instance, uint32_t *domain, int initial, int32_t application_rc)
{
    const ps3dk_prof_auto_options defaults=PS3DK_PROF_AUTO_OPTIONS_INIT;
    ps3dk_prof_auto_options normalized;
    const ps3dk_prof_auto_options *o=&normalized;
    auto_ring_plan ring;
    uint32_t ring_label=0;
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
    if(!options_copy(options ? options:&defaults,&normalized) ||
       !layout(o,&ring,&ring_label,&records,&frames,&bytes)) return rc;
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
       gpp_prepare(&next->clock_port,device(),gc,o->report_first,16,o->clock_label)!=PRG_OK ||
       (ring.blocks && gpp_prepare(&next->ring_port,device(),gc,ring.first,2u*ring.blocks*ring.pairs,ring_label)!=PRG_OK)) {
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
    if(!ring.blocks) {
        (void)auto_queue_init(&next->operations,(auto_queue_entry *)((uint8_t *)next+records),
                              o->metadata_records,(o->report_count-16)/2);
        queue_reserves(next,o,&ring);
    }
    next->ring_label=ring_label;next->scan=(uintptr_t)(void *)gc->current;
    for(uint32_t i=0;i<AUTO_SEGMENT_POOL;i++) next->segment_pool[i].next=i+1<AUTO_SEGMENT_POOL ? i+1:AUTO_SEGMENT_NONE;
    next->segment_free=0;next->segment_pending=AUTO_SEGMENT_NONE;
    gpp_bind(&next->operations_device,&next->operations_port);
    gpp_bind(&next->clock_device,&next->clock_port);
    if(ring.blocks) gpp_bind(&next->ring_device,&next->ring_port);
#ifdef PS3DK_PROF_AUTO_RING
    /* Before the lease: a failed override leaves the current reservation and capture intact. */
    if(ring.blocks && (reason=ring_attach(next,o,&ring))!=0) {
        rc=reason==AUTO_ALLOCATION_FAILED ? PS3DK_PROF_ENOMEM:PS3DK_PROF_EINVAL;goto fail;
    }
#endif
    if(p_ext_lock_at(p)!=0) {reason=AUTO_STOPPED;rc=PS3DK_PROF_ESTOPPED;goto fail;}
    locked=1;
    rc=p_auto_lease_claim_ring(p,gc,o->report_first,o->report_count,o->operation_label,o->clock_label,ring_label,
                               &id,&lease);
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
    config.pairs=(o->report_count-16)/2;config.metadata=ring.blocks ? ring.records:o->metadata_records;
    config.frames=o->frame_records;
    config.call_pairs=call_pairs(legacy_pool(o,&ring));(void)range_pairs(o,&ring,&config.range_pairs);
    if(ring.blocks) {
        /* REPORT_RING budgets: raw MAIN extents and metadata, each bounded separately */
        config.ring_bytes=ring.bytes;config.ring_blocks=ring.blocks;config.ring_pairs=ring.pairs;
        config.ring_first=ring.first;config.ring_label=ring_label;config.ring_stride=ring.stride;
        config.ring_slots=ring.slots;config.metadata_stride=ring.metadata_stride;config.legacy_pairs=ring.legacy_pairs;
    }
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

/* Owner only: recycling stops for the rest of the capture. A pending block is quarantined by its caller. */
static void ring_refuse(auto_capture *a, uint32_t reason)
{
    if(!a->ring_refusal) a->ring_refusal=reason;
    auto_ring_stop(&a->ring);
}

static int initialize_device(auto_capture *a)
{
    const auto_wire_config *c=auto_config_read(&configuration);
    if(a->device_initialized) return 1;
    if(!c || !p_auto_lease_mark(g_prof,a->lease_ticket)) return 0;
    a->operations_device.write_label(a->operations_device.ctx,c->op_label,0);
    a->clock_device.write_label(a->clock_device.ctx,c->clock_label,0);
    if(a->ring_plan.blocks) a->ring_device.write_label(a->ring_device.ctx,a->ring_label,0);
    a->operations_device.sync(a->operations_device.ctx);
    /* The ring's publication targets 2g-1, 2g only increase from this reset. A label that does not read back zero
     * stops recycling; the legacy pool is unaffected. */
    if(a->ring_plan.blocks && a->ring_device.read_label(a->ring_device.ctx,a->ring_label)!=0) ring_refuse(a,AUTO_RING_REFUSED_LABEL);
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
static int clock_emit(void *user,uint32_t report,uint32_t k)
{
    auto_capture *a=user;
    uintptr_t begin=(uintptr_t)(void *)a->context->begin,current=(uintptr_t)(void *)a->context->current,
              end=(uintptr_t)(void *)a->context->end;
    /* Twelve words, no reserve callback or special tail. This boundary may refuse
     * an anchor; it cannot expand or wrap the application's command extent. The
     * report's final timestamp is written after the label reaches 2k-1 and before
     * it reaches 2k. */
    if(begin>current || current>end || (current&3u) || end-current<48 || !initialize_device(a)) return 0;
    a->clock_device.write_timer(a->clock_device.ctx,report,PRG_POISON);
    a->clock_device.sync(a->clock_device.ctx);
    if(a->clock_device.reserve_pair(a->clock_device.ctx,report,a->clock_port.label,2u*k-1u)!=0) return 0;
    return a->clock_device.reserve_pair(a->clock_device.ctx,report,a->clock_port.label,2u*k)==0 ? 1:2;
}
static void clock_flush(void *user) {auto_capture *a=user;a->clock_device.flush(a->clock_device.ctx);}
static uint32_t clock_label(void *user) {auto_capture *a=user;return a->clock_device.read_label(a->clock_device.ctx,a->clock_port.label);}
static uint64_t clock_timer(void *user,uint32_t report) {auto_capture *a=user;return a->clock_device.read_timer(a->clock_device.ctx,report);}
static void clock_sync(void *user) {auto_capture *a=user;a->clock_device.sync(a->clock_device.ctx);}
/* Drainer side: reading an anchor the RSX already wrote needs only an initialized device and a configuration
 * nothing invalidated (a stop or seal still lets the final poll read it); never the owner's context pointers. */
static int clock_drain_live(void *user)
{
    auto_capture *a=user;
    return a->device_initialized && !auto_config_flags(&configuration);
}
static void sample_clock(auto_capture *a,uint64_t frame)
{
    const auto_clock_port port={a,clock_tb,clock_live,clock_emit,clock_flush,clock_label,clock_timer,clock_sync};
    g_tls.in_gpu=1;
    (void)auto_clock_sample(&a->clocks,&port,a->owner_incarnation,frame);
    /* the drainer now sets invalid (it invalidates the configuration itself); read it atomically here */
    if(LD_ACQ(&a->clocks.invalid)) {auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&a->operations);}
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
                     (index==a->operations_port.label || index==a->clock_port.label ||
                      (a->ring_port.ready && index==a->ring_port.label));
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

/* Saturating per-build counters: a positive count never wraps back to zero. */
static void history_add(uint32_t *v,uint32_t n)
{
    *v=n>UINT32_MAX-*v ? UINT32_MAX:*v+n;
}
/* A new build of a part starts: its CALL and report history starts again too. */
static void segment_new_build(auto_segment *s)
{
    s->visits=s->refused=s->called=s->reports=0;
    s->untimed_refused=s->other_refused=s->untimed_mixed=0;s->untimed_frame=0;s->fp=0;
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
        } else {
            /* a rebuild that cannot record its dropped build for lack of metadata latches the frame tail too */
            if(a->operations.refusal==AUTO_QUEUE_METADATA) a->pool_tail_column=4;
            auto_queue_quarantine_pair(&a->operations,b->pair);
        }
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
    segment_new_build(s);
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
    segment_new_build(s);
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
/* A refused segment draw: counted like any exclusion; the column is kept for the build's refusal history. */
static uint32_t seg_refuse(auto_capture *a,uint32_t kind,uint32_t column)
{
    a->admit_column=auto_frames_count(&a->coverage,kind,column) ? column:0;
    return 0;
}
static uint32_t segment_admit(auto_capture *a,auto_segment *s,gpp_ctx *ctx,const ps3tc_auto_call *call,
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
        return seg_refuse(a,call->kind,LD_ACQ(&g_prof->stop_latch) ? 7:9);
    }
    if(a->timing && !a->timed_open) {
        /* timing limited to timed ranges and none open: covered, deliberately untimed */
        a->admit_column=auto_frames_outside(&a->coverage,call->kind) ? 12u:0u;
        return 0;
    }
    if(a->pool_tail_column) {return seg_refuse(a,call->kind,a->pool_tail_column);}
    words=auto_command_words(call);
    if(!words) {return seg_refuse(a,call->kind,6);}
    if(!cur || (cur&3u) || cur>end || words>UINT32_MAX-4u || words+4u>(uint64_t)(end-cur)/4u) {
        return seg_refuse(a,call->kind,2);
    }
    if(a->segment_free==AUTO_SEGMENT_NONE) {
        a->pool_tail_column=4;return seg_refuse(a,call->kind,4);
    }
    if(!initialize_device(a)) {return seg_refuse(a,call->kind,10);}
    pair=auto_queue_take_pair(&a->operations);
    if(pair<0) {
        uint32_t column=a->operations.refusal==AUTO_QUEUE_STOPPED ? 9:3;
        if(column==3) a->pool_tail_column=3;
        return seg_refuse(a,call->kind,column);
    }
    a->segment_pending=pool_take(a);
    b=&a->segment_pool[a->segment_pending];memset(b,0,sizeof *b);b->pair=(uint32_t)pair;b->next=AUTO_SEGMENT_NONE;
    b->op.kind=(uint16_t)call->kind;b->op.caller=caller;b->op.generation=c->generation;
    b->op.flags=AUTO_OP_SEGMENT;b->op.segment=s->ordinal;
    b->op.tag=a->tag;b->op.fp_offset=s->fp;   /* the builder's tag, the part's own program */
    /* the build token: the coverage frame that counted this draw (sampled per draw, so a build that spans a
     * frame boundary names both frames) */
    if(a->coverage.active) {
        b->op.build_flags=AUTO_OP_BUILD_KNOWN;b->op.build_owner=a->owner_incarnation;
        b->op.build_frame=a->coverage.live.frame;
    }
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
    s->reports=1; /* a report write now lives in this build's buffer: a second CALL would replay it */
    a->expected_end=(uintptr_t)(void *)ctx->current+words*4;
    a->segment_active=s;a->segment_active_writer=ctx;g_tls.in_gpu=1;
    return 1;
}
/* Every covered emitter of a build is a visit of that build; one it does not time is also refused, so a CALL
 * that executes it knows the build is not fully represented by its records. */
static uint32_t segment_begin(auto_capture *a,auto_segment *s,gpp_ctx *ctx,const ps3tc_auto_call *call,
                              uint64_t caller)
{
    uint32_t ticket;
    history_add(&s->visits,1);
    a->admit_column=0;
    ticket=segment_admit(a,s,ctx,call,caller);
    if(!ticket) {
        history_add(&s->refused,1);
        /* untimed-only exclusions are columns 2..6 (room, reports, metadata, reentry, unsupported arguments), and 12
         * names a draw left untimed outside the timed ranges */
        if((a->admit_column>=2 && a->admit_column<=6) || a->admit_column==12) {
            if(s->untimed_refused && s->untimed_frame!=a->coverage.live.frame) s->untimed_mixed=1;
            s->untimed_frame=a->coverage.live.frame;history_add(&s->untimed_refused,1);
        } else history_add(&s->other_refused,1);
    }
    return ticket;
}

/* The tracked part a writer context is in at the end of an emitter, open or already closed: a Begin/End part
 * whose embedded context is the writer, else an external part whose claimed buffer holds the cursor (a part
 * switch in an overflow callback leaves the cursor in the new part; a closed part is still the buffer its CALL
 * will execute). NULL when the cursor is in no tracked part. */
static auto_segment *segment_at(auto_capture *a,gpp_ctx *ctx)
{
    uintptr_t cur=(uintptr_t)(void *)ctx->current;
    auto_segment *edge=NULL,*s=segment_find(a,ctx);
    if(s && !s->external) return s;
    for(uint32_t i=0;i<a->segments_used;i++) {
        s=&a->segments[i];
        if(!s->external) continue;
        if(cur>=s->lo && cur<s->hi) return s;
        if(cur==s->hi && !edge) edge=s;   /* a full buffer: the cursor rests on its end */
    }
    return edge;
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
         * refused it): the begin report may run; no end report is written.
         * The draw is in no build's pool: the part it began in and, after a
         * part switch, the part the writer is in now both count it as
         * refused (the destination as a visit too), so a CALL of either is
         * not taken as fully recorded. A cursor in no tracked part leaves
         * the words in an untracked buffer, whose CALL counts as untracked. */
        auto_segment *d=segment_at(a,ctx);
        auto_queue_quarantine_pair(&a->operations,b->pair);
        ST_REL(&a->segment_quarantine,1u);
        (void)auto_frames_count(&a->coverage,b->op.kind,11);
        pool_give(a,slot);
        history_add(&s->refused,1);history_add(&s->other_refused,1);
        if(d && d!=s) {history_add(&d->visits,1);history_add(&d->refused,1);history_add(&d->other_refused,1);}
        return;
    }
    segment_report(ctx,b->op.report_end);
    b->op.tb_after=observation_tb();
    if(b->op.tb_after<b->op.tb_before) b->op.flags|=8;
    (void)auto_frames_count(&a->coverage,b->op.kind,1);
    segment_append(a,s,slot);
}

/* cellGcmSegmentCall (observed build), before its CALL: a build that is not fully represented by its own records
 * (some covered draw of it was refused) gets a whole-CALL bracket instead, one report pair from the reserve kept for
 * them: the begin timestamp report here, in the ring before the CALL, and the end report after the retirement label
 * (ps3dk_prof__auto_segment_call), completed by the same operation-label release as the build's records. Only the
 * first CALL of a build, on the attached ring, inside a coverage frame and outside any draw bracket. Call reserved
 * the 2 words here together with its own; nothing here allocates, flushes or calls a callback. */
void ps3dk_prof__auto_segment_call_begin(struct _gcmCtxData *ring,struct _gcmCtxData *segment)
{
    auto_capture *a;
    auto_segment *s;
    const auto_wire_config *c;
    gpp_ctx *ctx=(gpp_ctx *)(void *)ring;
    uintptr_t cur,end;
    int pair;
    if(!ring || !segment || !p_ready() || g_tls.in_gpu || !p_window_gate(g_prof) || !(a=owner_capture(NULL,1))) return;
    if(!(s=segment_find(a,segment)) || s->called || s->bracket || !s->refused) return;
    c=auto_config_read(&configuration);
    cur=(uintptr_t)(void *)ctx->current;end=(uintptr_t)(void *)ctx->end;
    /* room: this report (2), the CALL and its label (5), the end report (2) and the release (4) */
    if(ctx!=a->context || !c || c->reason || a->active || a->segment_active || !a->coverage.active ||
       a->call_ordinal==UINT32_MAX || LD_ACQ(&g_prof->stop_latch) || !auto_config_can_admit(&configuration) ||
       (uintptr_t)(void *)ctx->begin!=clean_begin || end!=clean_end || (cur&3u) || cur>end || end-cur<52 ||
       !initialize_device(a) || (pair=auto_queue_take_reserved_pair(&a->operations))<0) {
        increment(a,&a->call_brackets_refused);return;
    }
    s->bracket=1;s->bracket_pair=(uint32_t)pair;
    a->operations_device.write_timer(a->operations_device.ctx,c->report_first+16+(uint32_t)pair*2,PRG_POISON);
    a->operations_device.write_timer(a->operations_device.ctx,c->report_first+17+(uint32_t)pair*2,PRG_POISON);
    a->operations_device.sync(a->operations_device.ctx);
    s->bracket_tb=observation_tb();
    segment_report(ctx,c->report_first+16+(uint32_t)pair*2);
    increment(a,&a->call_brackets);
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
    int meta_short;
    uint32_t bracket;
    uintptr_t cur,end;
    uint32_t lost=0;
    if(!ring || !segment || !p_ready() || !(a=owner_capture(NULL,1))) return;
    /* CALL completeness (COVERAGE segment_unrepresented): each case below adds, once, the covered draws this CALL
     * executes that will have no committed record carrying its token. */
    if(g_tls.in_gpu) {
        /* A CALL emitted inside an open draw bracket (from the writer's own callback): the pending operation is not
         * touched and nothing is committed, but the CALL has executed its build, so it is accounted conservatively.
         * Its report writes may land on pairs no record of this call owns, so any build with report writes sets
         * the capture-wide replay bit, first CALL or not; a later CALL of the same build is a replay. */
        if(!(s=segment_find(a,segment))) {(void)auto_frames_unrepresented(&a->coverage,1);return;}
        if(s->reports!=0) ST_REL(&a->segment_replay,1u);
        (void)auto_frames_unrepresented(&a->coverage,s->visits);
        s->called=1;
        return;
    }
    if(!(s=segment_find(a,segment))) {
        /* untracked (never seen, or refused at the table): its draws are unknown, so at least one */
        (void)auto_frames_unrepresented(&a->coverage,1);return;
    }
    if(s->called) {
        /* this build was already executed: the CALL replays it, report writes included */
        if(s->reports) ST_REL(&a->segment_replay,1u);
        (void)auto_frames_unrepresented(&a->coverage,s->visits);return;
    }
    if(!p_window_gate(g_prof)) {
        /* A CALL while no capture window is open (an immediate window ended earlier in this frame): it gets no
         * in-window execution token and emits nothing.  Its build is accounted as executed without a record: a
         * bracket begun for it is quarantined, its draws count as unrepresented, and the build is dropped, which
         * records it as a dropped build and quarantines every report pair its embedded timestamp writes target,
         * so neither this CALL nor a later replay can make stale data qualify.  The build counts as called, so a
         * later CALL of it is a replay: its draws are unrepresented and, when it holds timestamp writes, the capture's
         * replay latch is set. */
        if(s->bracket) {auto_queue_quarantine_pair(&a->operations,s->bracket_pair);ST_REL(&a->segment_quarantine,1u);}
        s->bracket=0;
        s->called=1;
        lost=s->count;history_add(&lost,s->refused);
        (void)auto_frames_unrepresented(&a->coverage,lost ? lost:1u);
        segment_drop(a,s);return;
    }
    s->called=1;
    bracket=s->bracket;s->bracket=0;
    if(!s->count && !bracket) {(void)auto_frames_unrepresented(&a->coverage,s->refused);return;}
    c=auto_config_read(&configuration);
    cur=(uintptr_t)(void *)ctx->current;end=(uintptr_t)(void *)ctx->end;
    meta_short=a->operations.capacity-(uint32_t)(a->operations.write_idx-LD_ACQ(&a->operations.read_idx))<
               s->count+(bracket ? 1u:0u);
    if(ctx!=a->context || !c || c->reason || a->active || a->call_ordinal==UINT32_MAX ||
       LD_ACQ(&g_prof->stop_latch) || !auto_config_can_admit(&configuration) ||
       (cur&3u) || cur>end || end-cur<(bracket ? 24u:16u) || meta_short) {
        /* Metadata exhaustion latches the frame tail like a refused admission does: later covered draws of this
         * frame are not timed either. A bracket's begin report is already in the ring: its pair is quarantined. */
        if(meta_short) a->pool_tail_column=4;
        if(bracket) {auto_queue_quarantine_pair(&a->operations,s->bracket_pair);ST_REL(&a->segment_quarantine,1u);}
        lost=s->count;history_add(&lost,s->refused);
        (void)auto_frames_unrepresented(&a->coverage,lost);
        segment_drop(a,s);return;
    }
    call=++a->call_ordinal;
    for(i=s->count ? s->head:AUTO_SEGMENT_NONE;i!=AUTO_SEGMENT_NONE;i=a->segment_pool[i].next) {
        auto_segment_op *b=&a->segment_pool[i];
        auto_queue_entry *e=auto_queue_claim_held(&a->operations,b->pair,share);
        uint64_t id;
        uint32_t sb,se;
        if(!e) {
            auto_queue_quarantine_pair(&a->operations,b->pair);ST_REL(&a->segment_quarantine,1u);
            history_add(&lost,1);continue;
        }
        id=e->op.id;sb=e->op.serial_begin;se=e->op.serial_end;
        e->op=b->op;e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;e->op.call=call;
        if(a->coverage.active) {e->op.flags|=1;e->op.frame_owner=a->owner_incarnation;e->op.frame=a->coverage.live.frame;}
        ST_REL(&a->admitted,id);
        auto_queue_commit(&a->operations);
        share=se;
    }
    segment_release(a,s);
    history_add(&lost,s->refused);
    (void)auto_frames_unrepresented(&a->coverage,lost);
    if(bracket) {
        /* The whole CALL: its end report after the retirement label, then one CALL record (kind 5) completed by
         * the release below with the build's records. args: draws of the build, draws of this CALL without a
         * record of their own (as counted into segment_unrepresented just above), segment and call ordinals, and
         * those refused with an untimed-only exclusion in one coverage frame (the build tail names it). */
        const uint32_t report=c->report_first+16+s->bracket_pair*2;
        auto_queue_entry *e;
        segment_report(ctx,report+1);
        e=a->coverage.active ? auto_queue_claim_held(&a->operations,s->bracket_pair,share):NULL;
        if(!e) {
            auto_queue_quarantine_pair(&a->operations,s->bracket_pair);ST_REL(&a->segment_quarantine,1u);
        } else {
            uint64_t id=e->op.id;
            uint32_t sb=e->op.serial_begin,se=e->op.serial_end;
            memset(&e->op,0,sizeof e->op);
            e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;
            e->op.kind=AUTO_KIND_CALL;e->op.generation=c->generation;e->op.tag=a->tag;e->op.fp_offset=a->fp_ring;
            e->op.flags=1;e->op.frame_owner=a->owner_incarnation;e->op.frame=a->coverage.live.frame;
            e->op.args[0]=s->visits;e->op.args[1]=lost;e->op.args[2]=s->ordinal;e->op.args[3]=call;
            if(s->untimed_refused && !s->untimed_mixed && !s->other_refused && s->untimed_refused<=lost &&
               s->untimed_frame<=a->coverage.live.frame) {
                e->op.args[4]=s->untimed_refused;
                e->op.build_flags=AUTO_OP_BUILD_KNOWN;e->op.build_owner=a->owner_incarnation;
                e->op.build_frame=s->untimed_frame;
            }
            e->op.report_begin=report;e->op.report_end=report+1;
            e->op.tb_before=s->bracket_tb;e->op.tb_after=observation_tb();
            if(e->op.tb_after<e->op.tb_before) e->op.flags|=8;
            ST_REL(&a->admitted,id);
            auto_queue_commit(&a->operations);
            share=se;
        }
    }
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
    e->op.tag=a->tag;e->op.fp_offset=a->fp_ring;
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

/* Outside a capture window: a covered draw written into a tracked segment build is not timed, but the build keeps
 * its accounting, as a visit refused for a reason other than room: a later CALL of the build inside a window then
 * gets a whole-CALL bracket or counts these draws as unrepresented, never as a build with no draws.  Nothing else
 * (no row count, no record, no report pair). */
static void unobserved_build(gpp_ctx *ctx,const ps3tc_auto_call *call)
{
    auto_capture *a;
    auto_segment *s;
    if(!call || call->kind<1 || call->kind>3 || g_tls.in_gpu || !(a=owner_capture(NULL,1)) || a->active ||
       a->segment_active) return;
    s=segment_find(a,ctx);
    if(!(s && s->open && !s->external)) s=external_writer(a,ctx);
    if(!s) return;
    history_add(&s->visits,1);history_add(&s->refused,1);history_add(&s->other_refused,1);
}

uint32_t ps3dk_prof__auto_begin(struct _gcmCtxData *context,const ps3tc_auto_call *call,uint64_t caller)
{
    gpp_ctx *ctx=(gpp_ctx *)(void *)context;
    if(!ctx || !p_ready()) return 0;
    if(!p_window_gate(g_prof)) {
        /* no draw timing outside a capture window; a stopped owner still acknowledges the stop here */
        if(LD_ACQ(&g_prof->stop_latch)) p_observe_stop(g_prof);
        else unobserved_build(ctx,call);
        return 0;
    }
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
    if(a->timing && !a->timed_open) { (void)auto_frames_outside(&a->coverage,call->kind);return 0; }
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
    e->op.tag=a->tag;e->op.fp_offset=a->fp_ring;
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

static int close_frame(auto_capture *a,uint32_t owner,uint64_t frame);
void p_auto_frame(uint64_t frame,uint64_t tb)
{
    auto_capture *a=owner_capture(NULL,1);
    if(a && !p_window_gate(g_prof)) {
        /* outside a window: the previous window's last row closes, clock anchors keep their cadence so the next
         * window's map qualifies, and no row or FRAME mark opens */
        if(!a->active && !g_tls.in_gpu && auto_config_can_admit(&configuration)) {
            if(a->coverage.active) (void)close_frame(a,a->coverage.owner,a->coverage.live.frame);
            if(frame%8u==0) sample_clock(a,frame);
        }
        return;
    }
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
    if(a->label_open) {
        a->label_spanning=1;
        if(a->coverage.active) add32(&f->label_spanning);
    }
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

/* Label-poll cadence (cellGcmIsLabelPassed / cellGcmIsLabelValuePassed in a PS3_PROF_AUTO unit): the flip-status
 * cadence rules for one label at a time. A poll of another label, a gap over the bound or a flip-wait scope ends the
 * open run as broken; a passed poll closes it as complete. Like flip-status cadence it shows how often the owner
 * looked, not that it had nothing else to do. label is the index, or 0xffffffff when the address is not a label slot:
 * such a poll has no identity, so it ends an open run as broken and never starts or completes one. Cadence is kept per
 * label within the row: label_one is the largest one-label total, the only value the reader's polling share uses. */
static void label_credit(auto_capture *a,auto_wire_frame *f,uint32_t id,uint64_t ticks)
{
    uint32_t i;
    if(a->label_row!=f->frame+1u) {a->label_row=f->frame+1u;a->label_n=0;}
    for(i=0;i<a->label_n && a->label_id[i]!=id;i++) {}
    if(i==a->label_n) {
        if(i==AUTO_LABEL_TRACK) return; /* not credited: never merged into another label's total */
        a->label_id[i]=id;a->label_sum[i]=0;a->label_n++;
    }
    add64(&a->label_sum[i],ticks);
    if(a->label_sum[i]>f->label_one) f->label_one=a->label_sum[i];
}
static void label_close(auto_capture *a,int complete)
{
    auto_wire_frame *f=&a->coverage.live;
    a->label_open=0;
    if(!a->coverage.active) return;
    if(a->label_spanning) add32(&f->label_spanning);
    else if(!complete) add32(&f->label_broken);
    else {
        uint32_t id=a->label_at+1u;
        add32(&f->label_runs);add64(&f->label_cadence,a->label_ticks);
        f->label_index=!f->label_index || f->label_index==id ? id:0xffffffffu;
        label_credit(a,f,id,a->label_ticks);
    }
}
static int label_gap(auto_capture *a,uint64_t before)
{
    uint64_t gap;
    if(before<a->label_last || (gap=before-a->label_last)>a->cadence_bound) return 0;
    add64(&a->label_ticks,gap);
    return 1;
}
uint64_t ps3dk_prof__auto_label_poll_begin(void)
{
    auto_capture *a;
    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return 0;
    return observation_tb()|1u;
}
void ps3dk_prof__auto_label_poll_end(uint64_t before,uint32_t label,int passed)
{
    auto_capture *a;
    if(!before || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return;
    if(a->label_open && a->label_at!=label) label_close(a,0);
    if(label==0xffffffffu) return; /* no identity: no run */
    if(!passed) {
        if(a->label_open && !label_gap(a,before)) label_close(a,0);
        if(!a->label_open) {a->label_open=1;a->label_spanning=0;a->label_ticks=0;a->label_at=label;}
        a->label_last=observation_tb();
        return;
    }
    if(a->label_open) label_close(a,label_gap(a,before));
}
uint64_t ps3dk_prof__auto_flip_wait_begin(void)
{
    auto_capture *a;
    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0)) || a->flip_wait_open) return 0;
    if(a->cadence_open) cadence_close(a,0); /* the scope takes over: the run never saw its done poll */
    if(a->label_open) label_close(a,0);
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
    if(a->coverage.live.flip_waits<2) {  /* where the first two scopes ran, for the reader's overlap with RSX busy */
        a->coverage.live.flip_wait_at[a->coverage.live.flip_waits][0]=a->flip_wait_since;
        a->coverage.live.flip_wait_at[a->coverage.live.flip_waits][1]=after;
    }
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
/* Owner thread only: a part's build history (visits, refused, called, reports), all zero when untracked. */
void ps3dk_prof__auto_test_segment_history(const void *key,uint32_t out[4])
{
    auto_segment *s=capture ? segment_find(capture,key):NULL;
    out[0]=s ? s->visits:0;out[1]=s ? s->refused:0;out[2]=s ? s->called:0;out[3]=s ? s->reports:0;
}
/* Owner thread only: preset a part's visits/refused counters (saturation tests). */
void ps3dk_prof__auto_test_segment_preset(const void *key,uint32_t visits,uint32_t refused)
{
    auto_segment *s=capture ? segment_find(capture,key):NULL;
    if(s) {s->visits=visits;s->refused=refused;}
}
/* Owner thread only: the open row's segment_unrepresented and flags, and the sticky replay latch. */
void ps3dk_prof__auto_test_unrepresented(uint32_t out[3])
{
    const auto_capture *a=capture;
    out[0]=a && a->coverage.active ? a->coverage.live.segment_unrepresented:0;
    out[1]=a && a->coverage.active ? a->coverage.live.flags:0;
    out[2]=a ? LD_ACQ(&a->segment_replay):0;
}
/* Owner thread only: preset the open row's segment_unrepresented (saturation tests). */
void ps3dk_prof__auto_test_unrepresented_preset(uint32_t v)
{
    if(capture && capture->coverage.active) capture->coverage.live.segment_unrepresented=v;
}
/* Owner thread only: the open row's label-poll block (runs, broken, spanning, label + 1, ticks, one-label ticks). */
void ps3dk_prof__auto_test_label(uint64_t out[6])
{
    const auto_wire_frame *f=capture && capture->coverage.active ? &capture->coverage.live:NULL;
    out[0]=f ? f->label_runs:0;out[1]=f ? f->label_broken:0;out[2]=f ? f->label_spanning:0;
    out[3]=f ? f->label_index:0;out[4]=f ? f->label_cadence:0;out[5]=f ? f->label_one:0;
}
/* Owner thread only: whole-CALL brackets begun, and CALLs that needed one and got none. */
void ps3dk_prof__auto_test_call_brackets(uint64_t out[2])
{
    const auto_capture *a=capture;
    out[0]=a ? a->call_brackets:0;out[1]=a ? a->call_brackets_refused:0;
}
/* Owner thread only: the frame's capacity-exhaustion latch (0, or the exclusion column it applies). */
uint32_t ps3dk_prof__auto_test_pool_tail(void)
{
    const auto_capture *a=capture;
    return a ? a->pool_tail_column:0;
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
    {
        const auto_clock_port port={capture,clock_tb,clock_drain_live,clock_emit,clock_flush,clock_label,clock_timer,
                                    clock_sync};
        if(!auto_clock_poll(&capture->clocks,&port,seal)) {
            auto_config_invalidate(&configuration,AUTO_STATUS_RESET);auto_queue_stop(&capture->operations);
        }
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
        if(LD_ACQ(&capture->segment_replay)) flags|=AUTO_STATUS_SEGMENT_REPLAY;
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

/* ---- tags, fragment programs, AUTO ranges (CONFIG TAG) ---------------------------------------------------------- */

/* PS3DK_PROF_AUTO_TAG on the owner thread: the tag every later operation records at its claim. Elsewhere inert. */
void ps3dk_prof_auto_tag(uint32_t tag)
{
    auto_capture *a;
    if(p_ready() && (a=owner_capture(NULL,0))) a->tag=tag;
}
/* PS3DK_PROF_AUTO_TIMED_RANGES_ONLY on the owner thread: from now on only covered draws inside a timed range get a
 * report pair; the others are covered and deliberately untimed (COVERAGE u64@416). */
void ps3dk_prof_auto_timing(uint32_t mode)
{
    auto_capture *a;
    if(p_ready() && (a=owner_capture(NULL,0))) a->timing=mode ? 1u:0u;
}
/* cellGcmSetFragmentProgram in a PS3_PROF_AUTO unit, after its FP_ADDRESS write: the program offset of that
 * context (the attached ring, or a part being built). No commands, no device access. */
void ps3dk_prof__auto_fragment_program(struct _gcmCtxData *context,uint32_t offset)
{
    auto_capture *a;
    auto_segment *s;
    if(!context || !p_ready() || !(a=owner_capture(NULL,0))) return;
    if((void *)context==(void *)a->context) {a->fp_ring=offset;return;}
    s=segment_find(a,context);
    if(!s || s->external) s=segment_at(a,(gpp_ctx *)(void *)context);
    if(s && s->open) s->fp=offset;
}
static void range_refuse(auto_capture *a)
{
    increment(a,&a->ranges_refused);
    if(a->coverage.active && a->coverage.live.ranges_refused!=UINT32_MAX) a->coverage.live.ranges_refused++;
}
/* BEGIN: a 2-word timestamp report in the attached ring (current room, no callback) from the range reserve, only
 * on the owner, inside a coverage frame, outside draw brackets and segment builds, at depth 4 or less. A refused
 * range is still pushed (or counted beyond the stack) so END stays balanced. 1 when recorded. */
int ps3dk_prof_auto_range_begin(ps3dk_prof_desc *desc,uint32_t timed)
{
    auto_capture *a;
    const auto_wire_config *c;
    gpp_ctx *ctx;
    auto_range *r;
    uint32_t idx=0,scope=0;
    uintptr_t cur,end;
    int pair;
    if(!desc || !p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0))) return 0;
    if(!p_window_gate(g_prof)) {   /* outside a window: refused, still pushed so END stays balanced */
        if(a->range_depth<AUTO_RANGE_DEPTH) {memset(&a->ranges[a->range_depth],0,sizeof a->ranges[0]);a->range_depth++;}
        else a->range_excess++;
        return 0;
    }
    if(a->range_depth>=AUTO_RANGE_DEPTH) {a->range_excess++;range_refuse(a);return 0;}
    r=&a->ranges[a->range_depth++];
    memset(r,0,sizeof *r);
    r->timed=timed ? 1u:0u;
    if(r->timed) a->timed_open++;  /* a timed range times its draws even when its own record is refused */
    c=auto_config_read(&configuration);ctx=a->context;
    cur=(uintptr_t)(void *)ctx->current;end=(uintptr_t)(void *)ctx->end;
    for(uint32_t i=0;i<a->segments_used;i++) if(a->segments[i].open) {range_refuse(a);return 0;} /* a segment build */
    if(!c || c->reason || a->active || a->segment_active || !a->coverage.active || LD_ACQ(&g_prof->stop_latch) ||
       !auto_config_can_admit(&configuration) || (uintptr_t)(void *)ctx->begin!=clean_begin || end!=clean_end ||
       (cur&3u) || cur>end || end-cur<8 || !initialize_device(a) ||
       p_gpu_desc(g_prof,desc,&idx,&scope)!=PS3DK_PROF_OK ||
       (pair=auto_queue_take_class_pair(&a->operations,AUTO_PAIR_RANGE))<0) {
        range_refuse(a);return 0;
    }
    r->ok=1;r->pair=(uint32_t)pair;r->desc=idx;r->tag=a->tag;r->frame=a->coverage.live.frame;
    a->operations_device.write_timer(a->operations_device.ctx,c->report_first+16+(uint32_t)pair*2,PRG_POISON);
    a->operations_device.write_timer(a->operations_device.ctx,c->report_first+17+(uint32_t)pair*2,PRG_POISON);
    a->operations_device.sync(a->operations_device.ctx);
    r->tb=observation_tb();
    segment_report(ctx,c->report_first+16+(uint32_t)pair*2);
    return 1;
}
/* END: the end report, one RANGE record, and the operation-label release that completes it (6 words, current room).
 * A range whose END falls in another frame (or without room, metadata or a valid capture) is refused: its begin
 * report is in the ring, so its pair is quarantined. 1 when committed. */
int ps3dk_prof_auto_range_end(void)
{
    auto_capture *a;
    const auto_wire_config *c;
    gpp_ctx *ctx;
    auto_range r;
    auto_queue_entry *e;
    uintptr_t cur,end;
    uint32_t report,depth;
    if(!p_ready() || g_tls.in_gpu || !(a=owner_capture(NULL,0))) return 0;
    if(a->range_excess) {a->range_excess--;return 0;}
    if(!a->range_depth) {range_refuse(a);return 0;}
    depth=a->range_depth;
    r=a->ranges[--a->range_depth];
    if(r.timed && a->timed_open) a->timed_open--;
    if(!r.ok) return 0;
    c=auto_config_read(&configuration);ctx=a->context;report=c->report_first+16+r.pair*2;
    cur=(uintptr_t)(void *)ctx->current;end=(uintptr_t)(void *)ctx->end;
    if(c->reason || a->active || a->segment_active || !a->coverage.active || a->coverage.live.frame!=r.frame ||
       LD_ACQ(&g_prof->stop_latch) || !auto_config_can_admit(&configuration) ||
       (uintptr_t)(void *)ctx->begin!=clean_begin || end!=clean_end || (cur&3u) || cur>end || end-cur<24 ||
       !(e=auto_queue_claim_held(&a->operations,r.pair,0))) {
        auto_queue_quarantine_pair(&a->operations,r.pair);ST_REL(&a->segment_quarantine,1u);
        range_refuse(a);return 0;
    }
    segment_report(ctx,report+1);
    {
        uint64_t id=e->op.id;
        uint32_t sb=e->op.serial_begin,se=e->op.serial_end,*w;
        memset(&e->op,0,sizeof e->op);
        e->op.id=id;e->op.serial_begin=sb;e->op.serial_end=se;
        e->op.kind=AUTO_KIND_RANGE;e->op.generation=c->generation;e->op.flags=1;
        e->op.frame_owner=a->owner_incarnation;e->op.frame=r.frame;
        e->op.args[0]=r.desc+1u;e->op.args[1]=r.timed;e->op.tag=r.tag;e->op.fp_offset=a->fp_ring;e->op.depth=depth;
        e->op.report_begin=report;e->op.report_end=report+1;
        e->op.tb_before=r.tb;e->op.tb_after=observation_tb();
        if(e->op.tb_after<e->op.tb_before) e->op.flags|=8;
        ST_REL(&a->admitted,id);
        auto_queue_commit(&a->operations);
        w=(uint32_t *)(void *)ctx->current;
        w[0]=(1u<<18)|0x1d6cu;w[1]=c->op_label*16u;   /* SEMAPHORE_OFFSET */
        w[2]=(1u<<18)|0x1d70u;                          /* BACKEND_WRITE_RELEASE */
        w[3]=(se&0xff00ff00u)|((se>>16)&0xffu)|((se&0xffu)<<16);
        ctx->current=(void *)(w+4);
        if(!auto_publication_commit(&a->publication,se)) auto_config_invalidate(&configuration,AUTO_STATUS_RESET);
    }
    increment(a,&a->ranges_done);
    return 1;
}
#ifdef PS3DK_PROF_TEST_HOOKS
/* The capture's two reserves (whole-CALL brackets, ranges). */
void ps3dk_prof__auto_test_reserves(uint32_t out[2])
{
    out[0]=capture ? capture->operations.cap[AUTO_PAIR_CALL]:0;out[1]=capture ? capture->operations.cap[AUTO_PAIR_RANGE]:0;
}
/* Owner thread only: ranges committed and refused, the open depth, and the tag. */
void ps3dk_prof__auto_test_ranges(uint64_t out[4])
{
    const auto_capture *a=capture;
    out[0]=a ? a->ranges_done:0;out[1]=a ? a->ranges_refused:0;out[2]=a ? a->range_depth:0;out[3]=a ? a->tag:0;
}
#endif
#if defined(PS3DK_PROF_TEST_HOOKS) && defined(PS3DK_PROF_AUTO_RING)
/* Host ring tests: the candidate or attached capture, its CONFIG bytes and the never-reusable retained pages. */
const auto_capture *ps3dk_prof__auto_test_capture(void) {return capture;}
const auto_wire_config *ps3dk_prof__auto_test_config(void) {return &configuration.config;}
uint64_t ps3dk_prof__auto_test_ring_retained(void) {return ring_retained;}
#endif
