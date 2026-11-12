/* hello-spu-gcm-segment, SPU side: build each frame's command segment in
 * local store with the SPU GCM writer and DMA it to main memory.
 *
 * arg1 = EA of the SegJob record, arg2 = EA of the SegDone record.
 * Each inbound mailbox word starts one job (0 ends the thread).  For a
 * job the SPU measures the command list, builds it into an exact-fit
 * buffer, DMAs it to job.segment_ea and only then writes SegDone, so the
 * PPU never sees a completion before the segment's bytes. */
#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/gcm/gcm_spu_cmd.h>
#include "../../source/job.h"

enum { TAG_JOB = 1, TAG_SEGMENT = 2, TAG_DONE = 3, LS_WORDS = 4096 };

static SegJob job __attribute__((aligned(128)));
static SegDone done __attribute__((aligned(128)));
static uint32_t ls[LS_WORDS] __attribute__((aligned(128)));

static void rect(CellGcmContextData *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb)
{
    cellGcmSetScissor(c, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    cellGcmSetClearColor(c, argb);
    cellGcmSetClearSurface(c, 0xf0);   /* R G B A */
}

/* The same picture the PPU sample draws, in SPU colours (magenta stripe). */
static void build(CellGcmContextData *c, const SegJob *j)
{
    uint32_t w = j->width, h = j->height, bar = w / 16, frame = j->frame - 1u;
    cellGcmSetBlendEnable(c, 0);
    cellGcmSetDepthTestEnable(c, 0);
    rect(c, 0, 0, w, h, 0xff181020);
    rect(c, (frame * 9u) % (w - bar), 48, bar, h - 96, j->slot ? 0xffc040c0 : 0xff902090);
    for (uint32_t n = 0; n < 3; ++n) rect(c, 16 + n * 80, 8, 64, 12, 0xffc040c0);
    rect(c, 16, h - 16, (w - 32) * (frame % 120u + 1u) / 120u, 8, 0xffc040c0);
    cellGcmSetWriteCommandLabel(c, (uint8_t)j->progress_label, frame);
    cellGcmSetScissor(c, (uint16_t)(w - 96), 8, 80, 24);   /* state for the ring's indicator */
    cellGcmSetClearColor(c, 0xff40e0ff);
}

static void put_done(uint64_t ea)
{
    mfc_put(&done, ea, sizeof(done), TAG_DONE, 0, 0);
    mfc_write_tag_mask(1u << TAG_DONE);
    mfc_read_tag_status_all();
}

int main(uint64_t job_ea, uint64_t done_ea, uint64_t arg3, uint64_t arg4)
{
    CellGcmContextData c;
    uint32_t tiny[16] __attribute__((aligned(16)));
    (void)arg3; (void)arg4;

    /* One-shot: a 64-byte buffer holds 7 two-word commands; End must refuse. */
    cellGcmSpuSetupContextData(&c, tiny, sizeof(tiny));
    for (uint32_t i = 0; i < 20; ++i) cellGcmSetClearColor(&c, i);
    done.overflow_refused = cellGcmSpuEnd(&c) == CELL_GCM_SPU_ERROR_OVERFLOW;
    done.overflow_words = cellGcmSpuGetOverflowWords(&c);

    for (;;) {
        uint32_t msg = spu_read_in_mbox();
        if (msg == 0) break;
        mfc_get(&job, job_ea, sizeof(job), TAG_JOB, 0, 0);
        mfc_write_tag_mask(1u << TAG_JOB);
        mfc_read_tag_status_all();

        cellGcmSpuSetupMeasureContext(&c);
        build(&c, &job);
        done.measured_bytes = cellGcmSpuGetUsedBytes(&c) + 4u;   /* + RETURN */
        done.used_bytes = 0;
        if (done.measured_bytes > job.segment_bytes || done.measured_bytes > sizeof(ls)) {
            done.status = 1;
        } else {
            cellGcmSpuSetupContextData(&c, ls, done.measured_bytes);   /* exact fit */
            build(&c, &job);
            if (cellGcmSpuEnd(&c) != CELL_OK) done.status = 2;
            else if (cellGcmSpuPutSegment(&c, job.segment_ea, TAG_SEGMENT) != CELL_OK) done.status = 3;
            else { done.status = 0; done.used_bytes = cellGcmSpuGetUsedBytes(&c) + 4u; }
        }
        /* The segment DMA completed above (its tag was waited on); only now
         * does the completion record go out. */
        done.frame = job.frame;
        put_done(done_ea);
    }
    done.frame = JOB_EXIT_FRAME;
    put_done(done_ea);
    sys_spu_thread_exit(0);
    return 0;
}
