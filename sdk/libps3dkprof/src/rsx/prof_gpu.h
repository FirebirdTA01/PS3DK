/* GPU (RSX) timestamp domains in the runtime (P3.3 R2; R4b target wiring): INTERNAL.  It is not installed under
 * sdk/include; the only target archive containing the GPU runtime is the UNINSTALLED ILP32 ext archive (make
 * ext-ilp32), where the device is GCM; host builds use a test-hook device.  Enabled by
 * ps3dk_prof_ext_config.gpu_domains_max (prof_spu.h).
 *
 * Every call: EBUSY inside a drain, a sink callback or another GPU call (port callbacks included), checked before any
 * lock; ENOTINIT before init; ENOTSUP without GPU domains (or without a device); ESTOPPED after a terminal extension
 * fault, after the shutdown claim (producer calls), or for a stopped domain (a lock fault or a detected anomaly: the
 * reason is in the stats).  The handle is the domain id (1026..1089, never reused); a stale handle is EINVAL.  Every
 * call on a domain except attach must come from the thread that attached it (the ctx owner), else EPERM, and must
 * pass the attached ctx, else EINVAL.  begin and mark register their descriptor through the calling thread's own
 * profiler incarnation: the ctx owner must be a registered thread (ps3dk_prof_thread_begin, or auto_register), else
 * ETHREAD. */
#ifndef PS3DK_PROF_GPU_H
#define PS3DK_PROF_GPU_H

#include <stdint.h>

#include <ps3dk/prof.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The GCM context: PSL1GHT's struct _gcmCtxData.  In C, <cell/gcm.h>'s CellGcmContextData and gcmContextData and
 * gCellGcmCurrentContext are all this type (no cast, full pointer checking); in C++ CellGcmContextData is a
 * distinct type DERIVED from it, so a CellGcmContextData* converts implicitly to this base pointer (not the reverse).
 * Declared, not included: usable before or after <cell/gcm.h>, and on the host with no cell headers. */
struct _gcmCtxData;
typedef struct _gcmCtxData ps3dk_prof_gcm_ctx;

/* A GPU zone.  Zero-initialised (PS3DK_PROF_GPU_ZONE_INIT) is INERT; ending an INERT zone is EINVAL. */
typedef struct ps3dk_prof_gpu_zone {
    uint32_t gpu, depth;
    uint64_t begin;
    uint32_t desc, pad;
} ps3dk_prof_gpu_zone;
#define PS3DK_PROF_GPU_ZONE_INIT {0u, 0u, 0u, 0u, 0u}

typedef struct ps3dk_prof_gpu_stats {
    /* producer (the ctx owner thread's counters) */
    uint64_t refused_api, refused_mark_depth, refused_suppress_overflow, refused_scope, refused_ctx, owner_busy;
    uint64_t lost_ring, lost_fifo, lost_records, brackets, cal_bad;
    /* drainer (copied under the extension mutex) */
    uint64_t events, lost, held, attribution_orphaned, brackets_dropped;
    uint32_t retired, final_written, final_state, fault, anomaly, loss_exhausted, overflow, pad;
} ps3dk_prof_gpu_stats;

/* report_first/report_count (2..gpu_report_slots_max) inside the report area and label inside the user label range
 * (named constants, pinned against GCM in R3); no overlap with another domain's ctx, reports or label (EINVAL);
 * EFULL: no free table slot or the 64 lifetime ids are used.  The context must be quiescent (an app precondition).
 * After a failed closing unlock: ESTOPPED with *gpu written (diagnostic only: no later call can lock). */
int ps3dk_prof_gpu_attach(ps3dk_prof_gcm_ctx *ctx, uint32_t report_first, uint32_t report_count,
                          uint32_t label, uint32_t *gpu);
/* out NULL: EINVAL, nothing written; any other failure writes an INERT zone; desc must be scope 2 (EINVAL). */
int ps3dk_prof_gpu_begin(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, ps3dk_prof_desc *desc,
                         ps3dk_prof_gpu_zone *out);
int ps3dk_prof_gpu_end(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, ps3dk_prof_gpu_zone *zone);
int ps3dk_prof_gpu_mark(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, ps3dk_prof_desc *desc);
/* NONE ({0, 0, 0}) or a token ps3dk_prof_frame_token() returned on a LIVE thread for a frame that thread began;
 * EBUSY when every owner entry is still leased. */
int ps3dk_prof_gpu_frame(uint32_t gpu, ps3dk_prof_token frame);
int ps3dk_prof_gpu_calibrate(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, uint64_t timeout_us);
/* Requests the detach and waits; the drain thread (or the designated APP thread's drains) finalizes it.  OK once
 * the finalizing drain turn completed, its closing extension unlock included: the domain is RETIRED (the frozen
 * terminal decision); whether the FINAL chunk was ACCEPTED is final_written in the stats (a failed sink retires
 * output-free).  ETIMEDOUT while pending (call again; a repeated call after OK is OK); ESTOPPED when that turn's
 * closing unlock failed or the seal won. */
int ps3dk_prof_gpu_detach(uint32_t gpu, ps3dk_prof_gcm_ctx *ctx, uint64_t timeout_us);
/* APP PRECONDITION before a successful release: the context is idle (cellGcmFinish or an equivalent the app owns)
 * and no CPU call on the domain is in flight.  A successful release returns the report range and label for
 * IMMEDIATE reuse by any attach; it proves neither that submitted GPU work completed nor that no GPU write is pending.
 * *released_or_null = 1 when the GPU resources were returned, also with ESTOPPED (a closing-unlock fault after the
 * release took effect: the table slot is quarantined).  EBUSY (released 0) while not retired, while a call is in
 * flight, or while a finalized detach's completion is still being acknowledged (this runtime rule is separate from the
 * seal's path and from the standalone core's prg_release). */
int ps3dk_prof_gpu_release(uint32_t gpu, uint32_t *released_or_null);
int ps3dk_prof_gpu_get_stats(uint32_t gpu, ps3dk_prof_gpu_stats *st);

#define PS3DK_PROF__GPU_DESC(var, nm) \
    static ps3dk_prof_desc var = {(nm), "", __FILE__, (uint32_t)__LINE__, 2u, 0u, 0u}

#if defined(PS3DK_PROF) && PS3DK_PROF

#define PS3DK_PROF_GPU_ZONE_BEGIN(gpu, ctx, name, zone)                                \
    (__extension__({                                                                    \
        PS3DK_PROF__GPU_DESC(ps3dk_prof__gd, name);                                    \
        ps3dk_prof_gpu_begin((gpu), (ctx), &ps3dk_prof__gd, &(zone));                  \
    }))
#define PS3DK_PROF_GPU_ZONE_END(gpu, ctx, zone) ps3dk_prof_gpu_end((gpu), (ctx), &(zone))
#define PS3DK_PROF_GPU_MARK(gpu, ctx, name)                                             \
    (__extension__({                                                                    \
        PS3DK_PROF__GPU_DESC(ps3dk_prof__gd, name);                                    \
        ps3dk_prof_gpu_mark((gpu), (ctx), &ps3dk_prof__gd);                            \
    }))

#else /* off build: nothing runs, no argument is evaluated, no output is written */

#define PS3DK_PROF_GPU_ZONE_BEGIN(gpu, ctx, name, zone)                                                              \
    ((void)sizeof(gpu), (void)sizeof(ctx), (void)sizeof(name), (void)sizeof(zone), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_GPU_ZONE_END(gpu, ctx, zone) ((void)sizeof(gpu), (void)sizeof(ctx), (void)sizeof(zone), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_GPU_MARK(gpu, ctx, name) ((void)sizeof(gpu), (void)sizeof(ctx), (void)sizeof(name), PS3DK_PROF_ENOTSUP)

#endif

#ifdef __cplusplus
}
#endif

#endif
