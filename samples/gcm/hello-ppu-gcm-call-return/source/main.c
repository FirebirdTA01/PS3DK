/* GCM command segments: each frame's drawing is built into a small command
 * segment and run from the ring with CALL; the segment ends in RETURN.
 * Two segments per pool are reused in turn, each only after the back-end
 * label written after its CALL has come back.  Phase 1 keeps the pool in
 * main memory mapped with cellGcmMapMainMemory, phase 2 in RSX local
 * memory.  Every wait is bounded.  No shaders, no input devices. */
#include <cell/gcm.h>
#include <cell/gcm/gcm_segment.h>
#include <cell/sysutil.h>
#include <sys/process.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "../../common/gcm_host_buffer.h"
#include "../../common/gcm_segment_display.h"
#include "../../common/gcm_segment_labels.h"

SYS_PROCESS_PARAM(1001, 0x100000);

enum {
    FRAMES        = 120,            /* per phase                              */
    FIFO_BYTES    = 1 << 20,
    HOST_BYTES    = 32 << 20,
    POOL_BYTES    = 2 << 20,        /* separately mapped main-memory pool     */
    SEG_BYTES     = 64 * 1024,
    LARGE_BYTES   = 1 << 20,        /* one-shot large segment                 */
    WAIT_POLLS    = 4000,           /* x 500 us = 2 s                         */
    WAIT_US       = 500,
    DECOY_TAG     = 0xdec00000u
};

static SegDisplay disp;
static volatile int quit_requested;

static void on_sysutil(uint64_t status, uint64_t param, void *data)
{
    (void)param; (void)data;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) quit_requested = 1;
}

static const uint8_t retire_label[2] = {SEG_LABEL_RETIRE_0, SEG_LABEL_RETIRE_1};
static const uint8_t progress_label[2] = {SEG_LABEL_PROGRESS0, SEG_LABEL_PROGRESS1};

static uint32_t label_value(uint8_t index)
{
    return *(volatile uint32_t *)cellGcmGetLabelAddress(index);
}

/* The frame's drawing, written into the segment.  The segment also leaves
 * scissor and clear colour set for the indicator box the ring clears after
 * RETURN, so the box shows whether state set inside a segment survives. */
static void build_frame(CellGcmContextData *c, unsigned phase, unsigned frame, unsigned slot)
{
    static const uint32_t stripe[2][2] = {{0xff30c080, 0xff208050}, {0xff5080ff, 0xff3050c0}};
    uint32_t w = disp.width, h = disp.height, bar = w / 16;
    seg_display_target(c, &disp);
    seg_rect(c, 0, 0, w, h, 0xff101824);
    seg_rect(c, (frame * 9u) % (w - bar), 48, bar, h - 96, stripe[phase][slot]);
    for (unsigned n = 0; n <= phase; ++n)                 /* phase bars */
        seg_rect(c, 16 + n * 80, 8, 64, 12, stripe[phase][0]);
    seg_rect(c, 16, h - 16, (w - 32) * (frame % FRAMES + 1) / FRAMES, 8, stripe[phase][0]);
    cellGcmSetWriteCommandLabel(c, progress_label[slot], frame);
    cellGcmSetScissor(c, w - 96, 8, 80, 24);              /* indicator box state */
    cellGcmSetClearColor(c, 0xffffd040);
}

/* A segment that writes only a decoy progress tag.  Placed at the IO offset
 * that equals a local segment's offset: if the front end reads a CALL target
 * as an IO offset, it runs this instead of the local segment. */
static int build_decoy(uint32_t *mem, uint32_t bytes, unsigned slot)
{
    CellGcmContextData c;
    cellGcmSetupContextData(&c, mem, bytes, NULL);
    cellGcmSetWriteCommandLabel(&c, progress_label[slot], DECOY_TAG | slot);
    *c.current++ = 0x00020000u;    /* RETURN */
    __sync_synchronize();
    return 1;
}

typedef struct {
    const char *name;
    unsigned frames, waits_polled, max_polls, decoy_hits, mismatches;
    uint32_t used_bytes;
} PhaseStats;

/* Check what the slot's previous segment left in its progress label. */
static void check_progress(PhaseStats *st, unsigned slot, uint32_t expect)
{
    uint32_t v = label_value(progress_label[slot]);
    if (v == expect) return;
    if (v == (DECOY_TAG | slot)) st->decoy_hits++;
    else {
        if (st->mismatches < 4)
            printf("GCM_SEG_NOTE phase=%s slot=%u progress=0x%08x expected=0x%08x\n",
                   st->name, slot, (unsigned)v, (unsigned)expect);
        st->mismatches++;
    }
}

static int run_phase(CellGcmContextData *ring, CellGcmSegment seg[2], unsigned phase,
                     PhaseStats *st)
{
    unsigned base = phase * FRAMES;
    for (unsigned i = 0; i < FRAMES; ++i) {
        unsigned frame = base + i, slot = frame & 1u;
        uint32_t polls = 0;
        int32_t rc;
        if (cellSysutilCheckCallback() || quit_requested) return 0;
        rc = cellGcmSegmentWaitRetired(&seg[slot], WAIT_POLLS, WAIT_US, &polls);
        if (rc != CELL_OK) {
            printf("GCM_SEG_FAIL phase=%s frame=%u retire wait rc=%08x polls=%u label=%u serial=%u\n",
                   st->name, frame, (unsigned)rc, polls, (unsigned)label_value(retire_label[slot]),
                   (unsigned)seg[slot].serial);
            return 0;
        }
        if (polls) st->waits_polled++;
        if (polls > st->max_polls) st->max_polls = polls;
        if (i >= 2) check_progress(st, slot, frame - 2);
        CellGcmContextData *c = cellGcmSegmentBegin(&seg[slot]);
        if (!c) { printf("GCM_SEG_FAIL phase=%s frame=%u begin refused\n", st->name, frame); return 0; }
        build_frame(c, phase, frame, slot);
        rc = cellGcmSegmentEnd(&seg[slot]);
        if (rc != CELL_OK) {
            printf("GCM_SEG_FAIL phase=%s frame=%u end rc=%08x overflow_words=%u\n", st->name,
                   frame, (unsigned)rc, cellGcmSegmentGetOverflowWords(&seg[slot]));
            return 0;
        }
        st->used_bytes = cellGcmSegmentGetUsedBytes(&seg[slot]);
        rc = cellGcmSegmentCall(ring, &seg[slot]);
        if (rc != CELL_OK) { printf("GCM_SEG_FAIL phase=%s frame=%u call rc=%08x\n", st->name, frame, (unsigned)rc); return 0; }
        /* The ring clears the indicator box with the scissor and colour the
         * segment left behind. */
        cellGcmSetClearSurface(ring, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        if (!seg_display_flip(ring, &disp, 2000000)) {
            printf("GCM_SEG_FAIL phase=%s frame=%u flip timeout\n", st->name, frame); return 0;
        }
        st->frames++;
    }
    /* Drain both slots and check the last two frames' progress. */
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (cellGcmSegmentWaitRetired(&seg[slot], WAIT_POLLS, WAIT_US, NULL) != CELL_OK) {
            printf("GCM_SEG_FAIL phase=%s drain slot=%u\n", st->name, slot); return 0;
        }
        check_progress(st, slot, base + FRAMES - 2 + slot);
    }
    return 1;
}

/* An overfull segment is refused, reported and never called. */
static int overflow_check(CellGcmContextData *ring, uint32_t *mem)
{
    CellGcmSegment s;
    uint32_t *before = ring->current;
    if (cellGcmSegmentInit(&s, mem, 256, CELL_GCM_LOCATION_MAIN, SEG_LABEL_RETIRE_L) != CELL_OK) return 0;
    CellGcmContextData *c = cellGcmSegmentBegin(&s);
    if (!c) return 0;
    for (unsigned i = 0; i < 64; ++i) cellGcmSetClearColor(c, i);
    int32_t end = cellGcmSegmentEnd(&s), call = cellGcmSegmentCall(ring, &s);
    int ok = end == CELL_GCM_SEGMENT_ERROR_OVERFLOW && call == CELL_GCM_SEGMENT_ERROR_OVERFLOW &&
             ring->current == before && cellGcmSegmentGetOverflowWords(&s) == 2u * 64u - 2u * 31u;
    printf("GCM_SEG_OVERFLOW %s capacity_words=63 refused_words=%u end=%08x call=%08x\n",
           ok ? "ok" : "FAIL", cellGcmSegmentGetOverflowWords(&s), (unsigned)end, (unsigned)call);
    return ok;
}

/* One large segment: LARGE_BYTES of NOPs and a marker label, then RETURN. */
static int large_check(CellGcmContextData *ring, uint32_t *mem)
{
    CellGcmSegment s;
    uint32_t polls = 0;
    if (cellGcmSegmentInit(&s, mem, LARGE_BYTES, CELL_GCM_LOCATION_MAIN, SEG_LABEL_RETIRE_L) != CELL_OK) return 0;
    CellGcmContextData *c = cellGcmSegmentBegin(&s);
    if (!c) return 0;
    *(volatile uint32_t *)cellGcmGetLabelAddress(SEG_LABEL_PROBE) = 0;
    uint32_t nops = LARGE_BYTES / 4 - 1 - 4;   /* RETURN slot, 4-word label */
    while (nops) { uint32_t n = nops > 1024 ? 1024 : nops; cellGcmSetNopCommand(c, n); nops -= n; }
    cellGcmSetWriteCommandLabel(c, SEG_LABEL_PROBE, 0x1a26e000u);
    if (cellGcmSegmentEnd(&s) != CELL_OK || cellGcmSegmentCall(ring, &s) != CELL_OK) return 0;
    cellGcmFlush(ring);
    int ok = cellGcmSegmentWaitRetired(&s, WAIT_POLLS, WAIT_US, &polls) == CELL_OK &&
             label_value(SEG_LABEL_PROBE) == 0x1a26e000u;
    printf("GCM_SEG_LARGE %s bytes=%u used=%u polls=%u marker=0x%08x\n", ok ? "ok" : "FAIL",
           LARGE_BYTES, cellGcmSegmentGetUsedBytes(&s), polls, (unsigned)label_value(SEG_LABEL_PROBE));
    return ok;
}

int main(void)
{
    sys_addr_t host = 0, pool = 0;
    uint32_t pool_offset = 0;
    CellGcmConfig config;
    CellGcmSegment main_seg[2], local_seg[2];
    PhaseStats st[2] = {{.name = "main"}, {.name = "local"}};
    const char *local_outcome = "LOCAL-SKIPPED";

    if (gcm_host_buffer_alloc(HOST_BYTES, &host) != 0 ||
        cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) || !seg_display_init(&disp)) {
        puts("GCM_SEG_FAIL initialization"); return 1;
    }
    CellGcmContextData *ring = CELL_GCM_CURRENT;
    if (cellSysutilRegisterCallback(0, on_sysutil, NULL)) { puts("GCM_SEG_FAIL sysutil"); return 1; }
    cellGcmGetConfiguration(&config);

    /* Phase-1 pool: separate 1 MiB-page main memory, mapped for the RSX. */
    if (gcm_host_buffer_alloc(POOL_BYTES, &pool) != 0 ||
        cellGcmMapMainMemory((void *)(uintptr_t)pool, POOL_BYTES, &pool_offset) != CELL_OK) {
        puts("GCM_SEG_FAIL map main memory"); return 1;
    }
    uint32_t *pool_mem = (uint32_t *)(uintptr_t)pool;
    printf("GCM_SEG_DISPLAY width=%u height=%u pool_io_offset=0x%08x\n",
           disp.width, disp.height, (unsigned)pool_offset);

    /* Pool layout: overflow probe at 0, large segment at 64 KiB, the two
     * frame segments after it. */
    uint8_t *pool_bytes = (uint8_t *)pool_mem;
    int ok = overflow_check(ring, pool_mem) &&
             large_check(ring, (uint32_t *)(pool_bytes + SEG_BYTES));
    for (unsigned k = 0; ok && k < 2; ++k)
        ok = cellGcmSegmentInit(&main_seg[k], pool_bytes + SEG_BYTES + LARGE_BYTES + k * SEG_BYTES,
                                SEG_BYTES, CELL_GCM_LOCATION_MAIN, retire_label[k]) == CELL_OK;
    if (!ok) { puts("GCM_SEG_FAIL main pool setup"); return 1; }
    for (unsigned k = 0; k < 2; ++k) *(volatile uint32_t *)cellGcmGetLabelAddress(progress_label[k]) = 0;
    if (!run_phase(ring, main_seg, 0, &st[0])) return 1;
    printf("GCM_SEG_PHASE name=main frames=%u calls=%u waits_polled=%u max_polls=%u bytes_per_segment=%u "
           "progress_mismatches=%u decoy=%u\n", st[0].frames, main_seg[0].calls + main_seg[1].calls,
           st[0].waits_polled, st[0].max_polls, st[0].used_bytes, st[0].mismatches, st[0].decoy_hits);

    /* Phase-2 pool: RSX local memory after the display buffers, 1 MiB
     * aligned.  Each local segment gets a decoy at the equal IO offset. */
    uint32_t local_off = (disp.local_used + 0xfffffu) & ~0xfffffu;
    void *decoy[2] = {NULL, NULL};
    int have_local = local_off + 2u * SEG_BYTES <= config.localSize;
    for (unsigned k = 0; have_local && k < 2; ++k) {
        uint32_t off = local_off + k * SEG_BYTES;
        if (cellGcmIoOffsetToAddress(off, &decoy[k]) != CELL_OK ||
            (uintptr_t)decoy[k] < (uintptr_t)host + FIFO_BYTES ||
            (uintptr_t)decoy[k] + SEG_BYTES > (uintptr_t)host + HOST_BYTES) have_local = 0;
    }
    if (have_local) {
        for (unsigned k = 0; k < 2; ++k) {
            build_decoy((uint32_t *)decoy[k], SEG_BYTES, k);
            void *mem = (void *)((uintptr_t)config.localAddress + local_off + k * SEG_BYTES);
            if (cellGcmSegmentInit(&local_seg[k], mem, SEG_BYTES, CELL_GCM_LOCATION_LOCAL,
                                   retire_label[k]) != CELL_OK) {
                puts("GCM_SEG_FAIL local pool setup"); return 1;
            }
            *(volatile uint32_t *)cellGcmGetLabelAddress(progress_label[k]) = 0;
        }
        if (!run_phase(ring, local_seg, 1, &st[1])) {
            puts("GCM_SEG_LOCAL outcome=LOCAL-STALLED");
            return 1;
        }
        if (st[1].decoy_hits == FRAMES && !st[1].mismatches) local_outcome = "LOCAL-IO-ALIAS";
        else if (!st[1].decoy_hits && !st[1].mismatches) local_outcome = "LOCAL-OK";
        else local_outcome = "LOCAL-MIXED";
        printf("GCM_SEG_PHASE name=local frames=%u local_offset=0x%08x waits_polled=%u max_polls=%u "
               "bytes_per_segment=%u progress_mismatches=%u decoy=%u outcome=%s\n", st[1].frames,
               (unsigned)local_off, st[1].waits_polled, st[1].max_polls, st[1].used_bytes,
               st[1].mismatches, st[1].decoy_hits, local_outcome);
    } else {
        printf("GCM_SEG_PHASE name=local outcome=LOCAL-SKIPPED local_offset=0x%08x\n", (unsigned)local_off);
    }
    cellSysutilUnregisterCallback(0);
    if (quit_requested) { puts("GCM_SEG_QUIT"); return 0; }
    if (st[0].mismatches || st[0].decoy_hits || !strcmp(local_outcome, "LOCAL-MIXED")) {
        printf("GCM_SEG_FAIL progress main_mismatches=%u local=%s\n", st[0].mismatches, local_outcome);
        return 1;
    }
    printf("GCM_SEG_PASS main_frames=%u local_frames=%u local_outcome=%s overflow=refused large_bytes=%u\n",
           st[0].frames, st[1].frames, local_outcome, LARGE_BYTES);
    return 0;
}
