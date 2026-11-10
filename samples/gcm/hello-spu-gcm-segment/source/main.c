/* SPU-built GCM command segments.  Each frame an SPU job writes the frame's
 * drawing into a main-memory command segment (libgcm_spu_cmd); the PPU
 * waits for the job's completion record, publishes the segment with CALL
 * from the ring and flips.  Two segments are reused in turn, each only
 * after its back-end retirement label.  The coherence recipe:
 *
 *   PPU  cellGcmSegmentWaitRetired  -> the RSX is done with the old contents
 *        cellGcmSegmentBeginExternal, write the job, mailbox the SPU
 *   SPU  build in local store, DMA the segment, wait for its tag,
 *        then DMA the completion record
 *   PPU  see the completion (bounded poll), sync,
 *        cellGcmSegmentEndExternal (checks the RETURN word), cellGcmSegmentCall
 *
 * Every wait is bounded and reports what it was waiting for. */
#include <cell/gcm.h>
#include <cell/gcm/gcm_segment.h>
#include <cell/sysutil.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../common/gcm_host_buffer.h"
#include "../../common/gcm_segment_display.h"
#include "../../common/gcm_segment_labels.h"
#include "job.h"
#include "hello_spu_gcm_segment_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

enum { FRAMES = 120, FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, POOL_BYTES = 1 << 20,
       SEG_BYTES = 16 * 1024, WAIT_POLLS = 4000, WAIT_US = 500 };

#define EA(p) ((u64)(uintptr_t)(p))

static SegDisplay disp;
static SegJob job __attribute__((aligned(128)));
static volatile SegDone done __attribute__((aligned(128)));
static const uint8_t retire_label[2] = {SEG_LABEL_RETIRE_0, SEG_LABEL_RETIRE_1};
static const uint8_t progress_label[2] = {SEG_LABEL_PROGRESS0, SEG_LABEL_PROGRESS1};

static int wait_done(uint32_t frame, uint32_t *polls_out)
{
    uint32_t polls = 0;
    while (done.frame != frame) {
        if (polls == WAIT_POLLS) { *polls_out = polls; return 0; }
        ++polls;
        usleep(WAIT_US);
    }
    __sync_synchronize();   /* read the record and the segment after the flag */
    *polls_out = polls;
    return 1;
}

int main(void)
{
    sys_addr_t host = 0, pool = 0;
    uint32_t pool_offset = 0, thread = 0, group = 0, cause = 0, status = 0;
    sysSpuImage image;
    CellGcmSegment seg[2];
    unsigned frames = 0, spu_polls_max = 0, retire_polled = 0, progress_bad = 0;
    uint32_t used = 0, measured = 0;
    int ok = 1;

    if (gcm_host_buffer_alloc(HOST_BYTES, &host) != 0 ||
        cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) || !seg_display_init(&disp) ||
        gcm_host_buffer_alloc(POOL_BYTES, &pool) != 0 ||
        cellGcmMapMainMemory((void *)(uintptr_t)pool, POOL_BYTES, &pool_offset) != CELL_OK) {
        puts("GCM_SPUSEG_FAIL initialization"); return 1;
    }
    CellGcmContextData *ring = CELL_GCM_CURRENT;
    for (unsigned k = 0; k < 2; ++k) {
        if (cellGcmSegmentInit(&seg[k], (uint8_t *)(uintptr_t)pool + k * SEG_BYTES, SEG_BYTES,
                               CELL_GCM_LOCATION_MAIN, retire_label[k]) != CELL_OK) {
            puts("GCM_SPUSEG_FAIL segment init"); return 1;
        }
        *(volatile uint32_t *)cellGcmGetLabelAddress(progress_label[k]) = 0;
    }

    if (sysSpuInitialize(6, 0) || sysSpuImageImport(&image, hello_spu_gcm_segment_bin, 0)) {
        puts("GCM_SPUSEG_FAIL spu image"); return 1;
    }
    sysSpuThreadGroupAttribute ga = { .nsize = 12, .name = (const char *)EA("gcm_seg_grp"), .type = 0 };
    sysSpuThreadAttribute ta = { .name = (const char *)EA("gcm_seg_spu"), .nsize = 12,
                                 .option = SPU_THREAD_ATTR_NONE };
    sysSpuThreadArgument arg = { .arg0 = EA(&job), .arg1 = EA(&done) };
    memset((void *)&done, 0, sizeof(done));
    if (sysSpuThreadGroupCreate(&group, 1, 100, &ga) ||
        sysSpuThreadInitialize(&thread, group, 0, &image, &ta, &arg) ||
        sysSpuThreadGroupStart(group)) {
        puts("GCM_SPUSEG_FAIL spu thread"); return 1;
    }
    printf("GCM_SPUSEG_DISPLAY width=%u height=%u pool_io_offset=0x%08x segment_bytes=%u\n",
           disp.width, disp.height, (unsigned)pool_offset, SEG_BYTES);

    for (unsigned f = 0; ok && f < FRAMES; ++f) {
        unsigned slot = f & 1u;
        uint32_t polls = 0;
        if (cellSysutilCheckCallback()) { ok = 0; break; }
        if (cellGcmSegmentWaitRetired(&seg[slot], WAIT_POLLS, WAIT_US, &polls) != CELL_OK) {
            printf("GCM_SPUSEG_FAIL frame=%u retire wait polls=%u\n", f, polls); ok = 0; break;
        }
        retire_polled += polls != 0;
        if (f >= 2 && *(volatile uint32_t *)cellGcmGetLabelAddress(progress_label[slot]) != f - 2) progress_bad++;
        void *buf = cellGcmSegmentBeginExternal(&seg[slot]);
        if (!buf) { printf("GCM_SPUSEG_FAIL frame=%u claim\n", f); ok = 0; break; }
        job.segment_ea = EA(buf);
        job.frame = f + 1u;
        job.slot = slot;
        job.width = disp.width; job.height = disp.height;
        job.segment_bytes = SEG_BYTES;
        job.progress_label = progress_label[slot];
        __sync_synchronize();   /* the job record is in memory before the mailbox */
        if (sysSpuThreadWriteMb(thread, f + 1u)) { printf("GCM_SPUSEG_FAIL frame=%u mailbox\n", f); ok = 0; break; }
        if (!wait_done(f + 1u, &polls)) {
            printf("GCM_SPUSEG_FAIL frame=%u spu completion timeout polls=%u seen=%u\n", f, polls,
                   (unsigned)done.frame);
            ok = 0; break;
        }
        if (polls > spu_polls_max) spu_polls_max = polls;
        if (done.status) { printf("GCM_SPUSEG_FAIL frame=%u spu status=%u measured=%u\n", f,
                                  (unsigned)done.status, (unsigned)done.measured_bytes); ok = 0; break; }
        used = done.used_bytes; measured = done.measured_bytes;
        int32_t rc = cellGcmSegmentEndExternal(&seg[slot], used);
        if (rc != CELL_OK) { printf("GCM_SPUSEG_FAIL frame=%u end rc=%08x used=%u\n", f, (unsigned)rc, used); ok = 0; break; }
        seg_display_target(ring, &disp);                  /* surface set in the ring */
        if (cellGcmSegmentCall(ring, &seg[slot]) != CELL_OK) { printf("GCM_SPUSEG_FAIL frame=%u call\n", f); ok = 0; break; }
        cellGcmSetClearSurface(ring, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);  /* indicator */
        if (!seg_display_flip(ring, &disp, SEG_FLIP_POLLS)) { printf("GCM_SPUSEG_FAIL frame=%u flip timeout\n", f); ok = 0; break; }
        ++frames;
    }
    for (unsigned k = 0; ok && k < 2; ++k) {
        if (cellGcmSegmentWaitRetired(&seg[k], WAIT_POLLS, WAIT_US, NULL) != CELL_OK) {
            printf("GCM_SPUSEG_FAIL drain slot=%u\n", k); ok = 0;
        } else if (*(volatile uint32_t *)cellGcmGetLabelAddress(progress_label[k]) != FRAMES - 2 + k) {
            progress_bad++;
        }
    }

    /* Stop the SPU: join only once it has said it is exiting. */
    uint32_t polls = 0;
    int exiting = sysSpuThreadWriteMb(thread, 0) == 0 && wait_done(JOB_EXIT_FRAME, &polls);
    if (exiting && (sysSpuThreadGroupJoin(group, &cause, &status) || sysSpuThreadGroupDestroy(group) ||
                    sysSpuImageClose(&image))) exiting = 0;
    printf("GCM_SPUSEG_SPU exit=%s cause=%u status=%u overflow_check=%s refused_words=%u\n",
           exiting ? "joined" : "TIMEOUT", cause, status, done.overflow_refused ? "refused" : "NOT-REFUSED",
           (unsigned)done.overflow_words);
    printf("GCM_SPUSEG_SUMMARY frames=%u spu_bytes=%u measured=%u max_spu_wait_polls=%u "
           "retire_waits_polled=%u progress_mismatches=%u\n", frames, used, measured, spu_polls_max,
           retire_polled, progress_bad);
    if (!ok || !exiting || frames != FRAMES || progress_bad || used != measured ||
        !done.overflow_refused || done.overflow_words != 26u) {
        puts("GCM_SPUSEG_FAIL verdict");
        return 1;
    }
    printf("GCM_SPUSEG_PASS frames=%u segments=2 spu_bytes_per_frame=%u exact_fit=yes overflow=refused\n",
           frames, used);
    return 0;
}
