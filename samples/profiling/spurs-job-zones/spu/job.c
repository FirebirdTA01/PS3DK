/* SPDX-License-Identifier: BSD-2-Clause
 * Explicit job-body zones only. No scheduler hooks or decrementer writes.
 * Runtime compatibility with SPURS atomic/event state is an unmeasured gate. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_context.h>
#include "protocol.h"
#include "prof_spu_lane.h"

/* Independently specified job metadata: heap 256 B, stack 8 KiB. */
__asm__(".pushsection .data\n.global _cell_spu_ls_param\n.align 4\n"
        ".type _cell_spu_ls_param, @object\n.size _cell_spu_ls_param, 16\n"
        "_cell_spu_ls_param:\n.long 256, 8192, 0, 0\n.popsection\n"
        ".pushsection .cell_spu_ls_param\n.long 256, 8192\n.popsection\n");

#if SJ_PROFILING
static struct {
    uint32_t before[32];
    ps3dk_prof_spu_writer writer;
    uint32_t after[32];
} writer_area __attribute__((aligned(128)));
static struct {
    uint32_t before[4];
    uint8_t bytes[PS3DK_PROF_SPU_STAGING_BYTES(SJ_LS_CAP)];
    uint32_t after[4];
} stage_area __attribute__((aligned(16)));
_Static_assert(offsetof(__typeof__(writer_area), writer) % 128 == 0, "writer alignment");
_Static_assert(offsetof(__typeof__(stage_area), bytes) % 16 == 0, "staging alignment");
#endif
static sj_receipt result;

static void barrier(void) { __asm__ volatile("" ::: "memory"); }
static uint32_t dec_read(void) { barrier(); uint32_t d=spu_readch(SPU_RdDec); barrier(); return d; }

static void result_put(uint64_t ea, uint32_t tag)
{
    uint32_t saved = mfc_read_tag_mask();
    barrier();
    mfc_put(&result, ea, sizeof result, tag, 0, 0);
    mfc_write_tag_mask(1u << tag);
    (void)mfc_read_tag_status_all();
    barrier();
    mfc_write_tag_mask(saved);
}

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    sj_receipt_reset(&result);
    if (!ctx || !job || ctx->dmaTag > 31) return; /* no valid result channel */
    const uint64_t *u = job->workArea.userData;
    if (!u[0] || (u[0] & 127u)) return;
    result.version=SJ_VERSION;
    result.job=(uint32_t)(u[5] >> 32); result.lane=(uint32_t)u[5];
    result.cookie=u[4]; result.ring=u[1]; result.ctl=u[2];
    result.iterations=(uint32_t)(u[6] >> 32);
    result.tag=ctx->dmaTag;
    result.mask_before=mfc_read_tag_mask();
    result.event_before=spu_readch(SPU_RdEventMask);
    result.guards=1;
    if (u[7] != SJ_VERSION || result.job >= SJ_JOBS ||
        result.iterations != SJ_ITERATIONS ||
        (uint32_t)u[3] != SJ_LS_CAP || (uint32_t)(u[3] >> 32) != SJ_RING_SLOTS) {
        result.status=1;
        goto publish;
    }
#if SJ_PROFILING
    for (unsigned i=0;i<32;++i) writer_area.before[i]=writer_area.after[i]=SJ_GUARD;
    for (unsigned i=0;i<4;++i) stage_area.before[i]=stage_area.after[i]=SJ_GUARD;
    result.writer_ls=(uint32_t)(uintptr_t)&writer_area.writer;
    result.writer_bytes=sizeof writer_area.writer;
    result.staging_ls=(uint32_t)(uintptr_t)stage_area.bytes;
    result.staging_bytes=sizeof stage_area.bytes;
    result.attach_rc=ps3dk_prof_spu_attach(&writer_area.writer, u[1], SJ_RING_SLOTS, u[2],
        ctx->dmaTag, stage_area.bytes, SJ_LS_CAP, 0); /* observe-only */
    if (result.attach_rc) { result.status=2; goto publish; }
    result.desc_count=writer_area.writer.desc_count;
    PS3DK_PROF_SPU_MARK(&writer_area.writer,"spurs.job.enter");
    uint32_t outer=PS3DK_PROF_SPU_ZONE_BEGIN(&writer_area.writer,"spurs.job");
    uint32_t inner=PS3DK_PROF_SPU_ZONE_BEGIN(&writer_area.writer,"spurs.work");
#else
    result.attach_rc=result.flush_rc=result.fini_rc=PS3DK_PROF_SPU_ENOTSUP;
#endif
    {
        volatile uint32_t x=0x13579bdfu ^ result.job;
        result.dec[0]=dec_read();
        for (uint32_t i=0;i<result.iterations/2;++i) x=sj_work_step(x,i);
        result.dec[1]=dec_read();
        for (uint32_t i=result.iterations/2;i<result.iterations;++i) x=sj_work_step(x,i);
        result.dec[2]=dec_read();
        result.checksum=x;
    }
#if SJ_PROFILING
    PS3DK_PROF_SPU_ZONE_END(&writer_area.writer,inner);
    PS3DK_PROF_SPU_MARK(&writer_area.writer,"spurs.checksum");
    PS3DK_PROF_SPU_ZONE_END(&writer_area.writer,outer);
    PS3DK_PROF_SPU_MARK(&writer_area.writer,"spurs.job.exit");
    result.flush_rc=ps3dk_prof_spu_flush(&writer_area.writer);
    result.fini_rc=ps3dk_prof_spu_fini(&writer_area.writer);
    result.post_fini=(result.fini_rc == 0);
    if (result.flush_rc || result.fini_rc) result.status=3;
    for (unsigned i=0;i<32;++i)
        if (writer_area.before[i]!=SJ_GUARD || writer_area.after[i]!=SJ_GUARD) result.guards=0;
    for (unsigned i=0;i<4;++i)
        if (stage_area.before[i]!=SJ_GUARD || stage_area.after[i]!=SJ_GUARD) result.guards=0;
#endif
    if (result.checksum != (uint32_t)u[6]) result.status=4;
publish:
    result.mask_after=mfc_read_tag_mask(); result.event_after=spu_readch(SPU_RdEventMask);
    if (!result.guards || result.mask_after!=result.mask_before || result.event_after!=result.event_before)
        result.status=5;
    result.magic=SJ_MAGIC; /* diagnostic presence; post_fini is a separate fact */
    result_put(u[0],ctx->dmaTag); /* wait before returning to the job runtime */
}
