/* SPDX-License-Identifier: BSD-2-Clause
 * Native big-endian PPU/SPU protocol; no pointers or ABI-sized fields. */
#ifndef SPURS_JOB_ZONES_PROTOCOL_H
#define SPURS_JOB_ZONES_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

#define SJ_JOBS 6u
#define SJ_VERSION 1u
#define SJ_MAGIC 0x534a5a31u
#define SJ_GUARD 0xa569c35au
#define SJ_LS_CAP 32u
#define SJ_RING_SLOTS 256u
#define SJ_ITERATIONS 65536u
#define SJ_NOT_CALLED (-999)

/* userData: result EA, ring EA, control EA, ring_slots:ls_cap,
 * cookie, job_id:lane_id, iterations:checksum, version. */
typedef struct sj_receipt {
    uint32_t magic, version, job, lane;
    uint64_t cookie, ring, ctl;
    int32_t attach_rc, flush_rc, fini_rc, status;
    uint32_t checksum, iterations, tag, mask_before, mask_after;
    uint32_t event_before, event_after, guards, post_fini;
    uint32_t dec[3];
    uint32_t writer_ls, writer_bytes, staging_ls, staging_bytes, desc_count, reserved;
} __attribute__((aligned(128))) sj_receipt;

#ifdef __cplusplus
static_assert(sizeof(sj_receipt) == 128, "one receipt line");
static_assert(offsetof(sj_receipt, cookie) == 16, "receipt cookie offset");
#else
_Static_assert(sizeof(sj_receipt) == 128, "one receipt line");
_Static_assert(offsetof(sj_receipt, cookie) == 16, "receipt cookie offset");
#endif

/* Explicit on every job entry, independent of whether a loader clears BSS.
 * Volatile bytes keep this standalone in a PIC job, without a libc call. */
static inline void sj_receipt_reset(sj_receipt *r)
{
    volatile unsigned char *p=(volatile unsigned char *)r;
    for (size_t i=0;i<sizeof *r;++i) p[i]=0;
    r->attach_rc=r->flush_rc=r->fini_rc=SJ_NOT_CALLED;
}

static inline uint32_t sj_work_step(uint32_t x, uint32_t i)
{
    return (x ^ (i + 0x9e3779b9u)) * 1664525u + 1013904223u;
}

static inline uint32_t sj_checksum(uint32_t job, uint32_t iterations)
{
    uint32_t x = 0x13579bdfu ^ job;
    for (uint32_t i = 0; i < iterations; ++i) x = sj_work_step(x, i);
    return x;
}

/* This predicate is used by the PPU before calling lane_end. Queue completion
 * must be independently established. A failed/missing fini receipt never
 * permits release, even if SPURS finalization returned successfully. */
static inline int sj_can_retire(int queue_complete, const sj_receipt *r,
                                uint32_t job, uint32_t lane, uint64_t cookie,
                                uint64_t ring, uint64_t ctl)
{
    return queue_complete && r && lane != 0 && r->magic == SJ_MAGIC &&
           r->version == SJ_VERSION && r->job == job && r->lane == lane &&
           r->cookie == cookie && r->ring == ring && r->ctl == ctl &&
           r->attach_rc == 0 && r->fini_rc == 0 && r->post_fini == 1;
}
#endif
