/* Nested-CALL probe.  The ring CALLs an outer segment that itself CALLs an
 * inner segment.  Command labels are written before the inner CALL (A),
 * inside the inner segment (I), after it (B) and in the ring after the
 * outer CALL (R).  One bounded wait, then the labels that were reached are
 * reported; nothing waits without a bound.
 *
 *   NESTED-OK              A I B R: the front end returned twice
 *   NESTED-STALLED-INNER   A I B, no R: the inner RETURN reused the one
 *                          return slot, the outer RETURN had none left
 *   NESTED-STALLED         A only: the inner CALL was not followed
 *   NESTED-OTHER           any other combination
 *
 * Before the probe, a ring-only label and a single-level CALL must both
 * complete (the controls); a failed control exits 1.  Any probe outcome
 * exits 0: the probe measures, it does not require nesting. */
#include <cell/gcm.h>
#include <cell/gcm/gcm_segment.h>
#include <sys/process.h>
#include <stdint.h>
#include <stdio.h>
#include "../../common/gcm_host_buffer.h"
#include "../../common/gcm_segment_display.h"
#include "../../common/gcm_segment_labels.h"

SYS_PROCESS_PARAM(1001, 0x100000);

enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, POOL_BYTES = 1 << 20, SEG_BYTES = 4096,
       WAIT_POLLS = 4000, WAIT_US = 500 };

static SegDisplay disp;

static volatile uint32_t *label(uint8_t index)
{
    return (volatile uint32_t *)cellGcmGetLabelAddress(index);
}

static void screen(CellGcmContextData *ring, uint32_t argb)
{
    seg_display_target(ring, &disp);
    seg_rect(ring, 0, 0, disp.width, disp.height, argb);
}

int main(void)
{
    sys_addr_t host = 0, pool = 0;
    uint32_t pool_offset = 0, polls = 0;
    CellGcmSegment outer, inner, single;

    if (gcm_host_buffer_alloc(HOST_BYTES, &host) != 0 ||
        cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) || !seg_display_init(&disp) ||
        gcm_host_buffer_alloc(POOL_BYTES, &pool) != 0 ||
        cellGcmMapMainMemory((void *)(uintptr_t)pool, POOL_BYTES, &pool_offset) != CELL_OK) {
        puts("GCM_NEST_FAIL initialization"); return 1;
    }
    CellGcmContextData *ring = CELL_GCM_CURRENT;
    uint8_t *mem = (uint8_t *)(uintptr_t)pool;
    for (uint8_t i = SEG_LABEL_NEST_A; i <= SEG_LABEL_WARM; ++i) *label(i) = 0;

    /* Control 1: the ring runs and writes a label; a frame is shown. */
    screen(ring, 0xff203050);
    cellGcmSetWriteCommandLabel(ring, SEG_LABEL_WARM, 1);
    if (!seg_display_flip(ring, &disp, 2000000) ||
        !seg_wait_label(SEG_LABEL_WARM, 1, WAIT_POLLS, WAIT_US, &polls)) {
        printf("GCM_NEST_FAIL control=ring warm=%u\n", (unsigned)*label(SEG_LABEL_WARM)); return 1;
    }

    /* Control 2: one level of CALL / RETURN. */
    if (cellGcmSegmentInit(&single, mem, SEG_BYTES, CELL_GCM_LOCATION_MAIN, SEG_LABEL_RETIRE_L) != CELL_OK) {
        puts("GCM_NEST_FAIL segment init"); return 1;
    }
    CellGcmContextData *c = cellGcmSegmentBegin(&single);
    cellGcmSetWriteCommandLabel(c, SEG_LABEL_NEST_I, 1);
    if (cellGcmSegmentEnd(&single) != CELL_OK || cellGcmSegmentCall(ring, &single) != CELL_OK) {
        puts("GCM_NEST_FAIL single segment"); return 1;
    }
    cellGcmSetWriteCommandLabel(ring, SEG_LABEL_NEST_R, 1);
    cellGcmFlush(ring);
    if (!seg_wait_label(SEG_LABEL_NEST_R, 1, WAIT_POLLS, WAIT_US, &polls) || *label(SEG_LABEL_NEST_I) != 1) {
        printf("GCM_NEST_FAIL control=single I=%u R=%u\n", (unsigned)*label(SEG_LABEL_NEST_I),
               (unsigned)*label(SEG_LABEL_NEST_R));
        return 1;
    }
    printf("GCM_NEST_CONTROL ring=ok single_call=ok polls=%u\n", polls);

    /* The probe: inner first, then the outer that CALLs it. */
    if (cellGcmSegmentInit(&inner, mem + SEG_BYTES, SEG_BYTES, CELL_GCM_LOCATION_MAIN,
                           SEG_LABEL_RETIRE_0) != CELL_OK ||
        cellGcmSegmentInit(&outer, mem + 2 * SEG_BYTES, SEG_BYTES, CELL_GCM_LOCATION_MAIN,
                           SEG_LABEL_RETIRE_1) != CELL_OK) {
        puts("GCM_NEST_FAIL probe init"); return 1;
    }
    c = cellGcmSegmentBegin(&inner);
    cellGcmSetWriteCommandLabel(c, SEG_LABEL_NEST_I, 2);
    int built = cellGcmSegmentEnd(&inner) == CELL_OK;
    c = cellGcmSegmentBegin(&outer);
    cellGcmSetWriteCommandLabel(c, SEG_LABEL_NEST_A, 2);
    cellGcmSetCallCommand(c, inner.offset);
    cellGcmSetWriteCommandLabel(c, SEG_LABEL_NEST_B, 2);
    built = built && cellGcmSegmentEnd(&outer) == CELL_OK && cellGcmSegmentCall(ring, &outer) == CELL_OK;
    if (!built) { puts("GCM_NEST_FAIL probe build"); return 1; }
    cellGcmSetWriteCommandLabel(ring, SEG_LABEL_NEST_R, 2);
    cellGcmFlush(ring);
    int r = seg_wait_label(SEG_LABEL_NEST_R, 2, WAIT_POLLS, WAIT_US, &polls);
    uint32_t a = *label(SEG_LABEL_NEST_A), i = *label(SEG_LABEL_NEST_I),
             b = *label(SEG_LABEL_NEST_B), rv = *label(SEG_LABEL_NEST_R);
    int A = a == 2, I = i == 2, B = b == 2, R = r && rv == 2;
    const char *outcome = (A && I && B && R) ? "NESTED-OK"
                        : (A && I && B && !R) ? "NESTED-STALLED-INNER"
                        : (A && !I && !B && !R) ? "NESTED-STALLED"
                        : "NESTED-OTHER";
    printf("GCM_NEST_LABELS A=%u I=%u B=%u R=%u reached=%s%s%s%s waited_polls=%u\n",
           (unsigned)a, (unsigned)i, (unsigned)b, (unsigned)rv,
           A ? "A" : "-", I ? "I" : "-", B ? "B" : "-", R ? "R" : "-", polls);

    /* Show the result if the front end still runs (bounded). */
    screen(ring, R ? 0xff30c060 : 0xffe08020);
    int shown = seg_display_flip(ring, &disp, 2000000);
    printf("GCM_NEST_OUTCOME outcome=%s post_flip=%s\n", outcome, shown ? "ok" : "timeout");
    return 0;
}
