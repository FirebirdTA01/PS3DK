/* GCM command segments: each frame's drawing is built into a small command
 * segment and run from the ring with CALL; the segment ends in RETURN.
 * Two segments are reused in turn, each only after the back-end label
 * written after its CALL has come back.  Every wait is bounded by a poll
 * count and names the label it waits for.  No shaders, no input devices.
 *
 * Variants (CMakeLists.txt builds one SELF per variant):
 *   default  overflow refusal (no CALL) + 120 frames from mapped main memory
 *   -local   overflow refusal + 120 frames from RSX local memory, with a decoy
 *            at the equal IO offset (GCM_SEG_VARIANT_LOCAL)
 *   -large   overflow refusal + one 1 MiB segment (GCM_SEG_VARIANT_LARGE)
 *   -jump    overflow refusal + 120 frames from mapped main memory, each a
 *            CALLed segment that JUMPs to a second sub-segment, which RETURNs
 *            (GCM_SEG_VARIANT_JUMP)
 * Progress goes to GCM_SEG_STEP lines and /dev_hdd0/tmp/gcmseg-crumb.txt. */
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
#include "../../common/gcm_segment_crumb.h"

#if defined(GCM_SEG_VARIANT_LOCAL)
#define VARIANT "local"
#elif defined(GCM_SEG_VARIANT_LARGE)
#define VARIANT "large"
#elif defined(GCM_SEG_VARIANT_JUMP)
#define VARIANT "jump"
#else
#define VARIANT "main"
#endif

SYS_PROCESS_PARAM(1001, 0x100000);

enum {
    FRAMES        = 120,
    FIFO_BYTES    = 1 << 20,
    HOST_BYTES    = 32 << 20,
    POOL_BYTES    = 2 << 20,        /* separately mapped main-memory pool     */
    SEG_BYTES     = 64 * 1024,
    LARGE_BYTES   = 1 << 20,        /* one-shot large segment                 */
    WAIT_POLLS    = 4000,           /* polls, 500 us apart                    */
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
 * RETURN, so the box shows whether state set inside a segment survives.
 * phase 0 (main memory): green stripe, one bar; phase 1 (local): blue, two. */
static const uint32_t stripe[2][2] = {{0xff30c080, 0xff208050}, {0xff5080ff, 0xff3050c0}};

/* First part: target, background, stripe.  The JUMP variant draws its
 * stripe in orange so the screen shows which variant ran. */
__attribute__((unused)) static void build_frame_head(CellGcmContextData *c, unsigned phase, unsigned frame, unsigned slot)
{
    uint32_t w = disp.width, h = disp.height, bar = w / 16;
    (void)phase;
    seg_display_target(c, &disp);
    seg_rect(c, 0, 0, w, h, 0xff101824);
#ifdef GCM_SEG_VARIANT_JUMP
    seg_rect(c, (frame * 9u) % (w - bar), 48, bar, h - 96, slot ? 0xffe08020 : 0xffc06010);
#else
    seg_rect(c, (frame * 9u) % (w - bar), 48, bar, h - 96, stripe[phase][slot]);
#endif
}

/* Second part: phase bars, progress bar, progress label, indicator state. */
__attribute__((unused)) static void build_frame_tail(CellGcmContextData *c, unsigned phase, unsigned frame, unsigned slot)
{
    uint32_t w = disp.width, h = disp.height;
    for (unsigned n = 0; n <= phase; ++n)                 /* phase bars */
        seg_rect(c, 16 + n * 80, 8, 64, 12, stripe[phase][0]);
    seg_rect(c, 16, h - 16, (w - 32) * (frame % FRAMES + 1) / FRAMES, 8, stripe[phase][0]);
    cellGcmSetWriteCommandLabel(c, progress_label[slot], frame);
    cellGcmSetScissor(c, w - 96, 8, 80, 24);              /* indicator box state */
    cellGcmSetClearColor(c, 0xffffd040);
}

__attribute__((unused)) static void build_frame(CellGcmContextData *c, unsigned phase, unsigned frame, unsigned slot)
{
    build_frame_head(c, phase, frame, slot);
    build_frame_tail(c, phase, frame, slot);
}

#ifdef GCM_SEG_VARIANT_LOCAL
/* A segment that writes only a decoy progress tag.  Placed at the IO offset
 * that equals a local segment's offset: if the front end reads a CALL target
 * as an IO offset, it runs this instead of the local segment. */
static void build_decoy(uint32_t *mem, uint32_t bytes, unsigned slot)
{
    CellGcmContextData c;
    cellGcmSetupContextData(&c, mem, bytes, NULL);
    cellGcmSetWriteCommandLabel(&c, progress_label[slot], DECOY_TAG | slot);
    *c.current++ = 0x00020000u;    /* RETURN */
    __sync_synchronize();
}
#endif

typedef struct {
    const char *name;
    unsigned frames, waits_polled, max_polls, decoy_hits, mismatches;
    uint32_t used_bytes;
} PhaseStats;

/* Check what the slot's previous segment left in its progress label. */
__attribute__((unused)) static void check_progress(PhaseStats *st, unsigned slot, uint32_t expect)
{
    uint32_t v = label_value(progress_label[slot]);
    if (v == expect) return;
    if (v == (DECOY_TAG | slot)) st->decoy_hits++;
    else {
        if (st->mismatches < 4)
            printf("GCM_SEG_NOTE phase=%s slot=%u label=%u progress=0x%08x expected=0x%08x\n",
                   st->name, slot, progress_label[slot], (unsigned)v, (unsigned)expect);
        st->mismatches++;
    }
}

__attribute__((unused)) static int wait_slot(PhaseStats *st, CellGcmSegment *s, unsigned slot, unsigned frame, uint32_t *polls)
{
    int32_t rc = cellGcmSegmentWaitRetired(s, WAIT_POLLS, WAIT_US, polls);
    if (rc == CELL_OK) return 1;
    printf("GCM_SEG_FAIL phase=%s frame=%u retire wait rc=%08x polls=%u label=%u value=%u serial=%u\n",
           st->name, frame, (unsigned)rc, *polls, retire_label[slot],
           (unsigned)label_value(retire_label[slot]), (unsigned)s->serial);
    seg_step("%s FAIL retire wait frame=%u label=%u value=%u serial=%u", st->name, frame,
             retire_label[slot], (unsigned)label_value(retire_label[slot]), (unsigned)s->serial);
    return 0;
}

/* The -large variant runs no frame phase. */
__attribute__((unused)) static int run_phase(CellGcmContextData *ring, CellGcmSegment seg[2], unsigned phase, PhaseStats *st)
{
    seg_step("%s begin frames=%u", st->name, FRAMES);
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        unsigned slot = frame & 1u;
        /* Every step of the first frames is a crumb; then one per 30 frames. */
        int loud = frame < 3 || frame % 30 == 0;
        uint32_t polls = 0;
        int32_t rc;
        if (cellSysutilCheckCallback() || quit_requested) return 0;
        if (!wait_slot(st, &seg[slot], slot, frame, &polls)) return 0;
        if (polls) st->waits_polled++;
        if (polls > st->max_polls) st->max_polls = polls;
        if (frame >= 2) check_progress(st, slot, frame - 2);
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
        if (loud) seg_step("%s frame=%u slot=%u call offset=0x%08x bytes=%u", st->name, frame, slot,
                           (unsigned)seg[slot].offset, st->used_bytes);
        rc = cellGcmSegmentCall(ring, &seg[slot]);
        if (rc != CELL_OK) { printf("GCM_SEG_FAIL phase=%s frame=%u call rc=%08x\n", st->name, frame, (unsigned)rc); return 0; }
        /* The ring clears the indicator box with the scissor and colour the
         * segment left behind. */
        cellGcmSetClearSurface(ring, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        if (!seg_display_flip(ring, &disp, SEG_FLIP_POLLS)) {
            printf("GCM_SEG_FAIL phase=%s frame=%u flip not done after %u polls (retire label %u = %u, serial %u)\n",
                   st->name, frame, SEG_FLIP_POLLS, retire_label[slot],
                   (unsigned)label_value(retire_label[slot]), (unsigned)seg[slot].serial);
            seg_step("%s FAIL flip frame=%u", st->name, frame);
            return 0;
        }
        if (loud) seg_step("%s frame=%u flipped", st->name, frame);
        st->frames++;
    }
    /* Drain both slots and check the last two frames' progress. */
    for (unsigned slot = 0; slot < 2; ++slot) {
        uint32_t polls = 0;
        if (!wait_slot(st, &seg[slot], slot, FRAMES + slot, &polls)) return 0;
        check_progress(st, slot, FRAMES - 2 + slot);
    }
    seg_step("%s end frames=%u decoy=%u mismatches=%u", st->name, st->frames, st->decoy_hits, st->mismatches);
    return 1;
}

#ifdef GCM_SEG_VARIANT_JUMP
/* Each frame: segment A (CALLed) draws the head and JUMPs to sub-segment B,
 * which draws the tail (including the progress label) and RETURNs.  B is
 * never CALLed itself; it is rebuilt only after A's retirement label, which
 * the back end writes after the RETURN that ends B. */
static int run_jump_phase(CellGcmContextData *ring, CellGcmSegment a[2], CellGcmSegment b[2], PhaseStats *st)
{
    seg_step("%s begin frames=%u", st->name, FRAMES);
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        unsigned slot = frame & 1u;
        int loud = frame < 3 || frame % 30 == 0;
        uint32_t polls = 0;
        int32_t rc;
        if (cellSysutilCheckCallback() || quit_requested) return 0;
        if (!wait_slot(st, &a[slot], slot, frame, &polls)) return 0;
        if (polls) st->waits_polled++;
        if (polls > st->max_polls) st->max_polls = polls;
        if (frame >= 2) check_progress(st, slot, frame - 2);
        CellGcmContextData *cb = cellGcmSegmentBegin(&b[slot]);
        if (!cb) { printf("GCM_SEG_FAIL phase=%s frame=%u sub-segment begin refused\n", st->name, frame); return 0; }
        build_frame_tail(cb, 0, frame, slot);
        if ((rc = cellGcmSegmentEnd(&b[slot])) != CELL_OK) {
            printf("GCM_SEG_FAIL phase=%s frame=%u sub-segment end rc=%08x\n", st->name, frame, (unsigned)rc);
            return 0;
        }
        CellGcmContextData *ca = cellGcmSegmentBegin(&a[slot]);
        if (!ca) { printf("GCM_SEG_FAIL phase=%s frame=%u begin refused\n", st->name, frame); return 0; }
        build_frame_head(ca, 0, frame, slot);
        cellGcmSetJumpCommand(ca, b[slot].offset);
        if ((rc = cellGcmSegmentEnd(&a[slot])) != CELL_OK) {   /* its RETURN is never reached */
            printf("GCM_SEG_FAIL phase=%s frame=%u end rc=%08x\n", st->name, frame, (unsigned)rc);
            return 0;
        }
        st->used_bytes = cellGcmSegmentGetUsedBytes(&a[slot]) + cellGcmSegmentGetUsedBytes(&b[slot]);
        if (loud) seg_step("%s frame=%u slot=%u call offset=0x%08x jump offset=0x%08x bytes=%u", st->name,
                           frame, slot, (unsigned)a[slot].offset, (unsigned)b[slot].offset, st->used_bytes);
        if ((rc = cellGcmSegmentCall(ring, &a[slot])) != CELL_OK) {
            printf("GCM_SEG_FAIL phase=%s frame=%u call rc=%08x\n", st->name, frame, (unsigned)rc);
            return 0;
        }
        cellGcmSetClearSurface(ring, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        if (!seg_display_flip(ring, &disp, SEG_FLIP_POLLS)) {
            printf("GCM_SEG_FAIL phase=%s frame=%u flip not done after %u polls (retire label %u = %u, serial %u)\n",
                   st->name, frame, SEG_FLIP_POLLS, retire_label[slot],
                   (unsigned)label_value(retire_label[slot]), (unsigned)a[slot].serial);
            seg_step("%s FAIL flip frame=%u", st->name, frame);
            return 0;
        }
        if (loud) seg_step("%s frame=%u flipped", st->name, frame);
        st->frames++;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        uint32_t polls = 0;
        if (!wait_slot(st, &a[slot], slot, FRAMES + slot, &polls)) return 0;
        check_progress(st, slot, FRAMES - 2 + slot);
    }
    seg_step("%s end frames=%u mismatches=%u", st->name, st->frames, st->mismatches);
    return 1;
}
#endif

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
    seg_step("overflow %s refused_words=%u (no CALL issued)", ok ? "ok" : "FAIL",
             cellGcmSegmentGetOverflowWords(&s));
    return ok;
}

#ifdef GCM_SEG_VARIANT_LARGE
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
    if (cellGcmSegmentEnd(&s) != CELL_OK) return 0;
    seg_step("large call offset=0x%08x bytes=%u", (unsigned)s.offset, cellGcmSegmentGetUsedBytes(&s));
    if (cellGcmSegmentCall(ring, &s) != CELL_OK) return 0;
    cellGcmFlush(ring);
    int retired = cellGcmSegmentWaitRetired(&s, WAIT_POLLS, WAIT_US, &polls) == CELL_OK;
    int ok = retired && label_value(SEG_LABEL_PROBE) == 0x1a26e000u;
    printf("GCM_SEG_LARGE %s bytes=%u used=%u polls=%u retire_label=%u retire_value=%u marker_label=%u marker=0x%08x\n",
           ok ? "ok" : "FAIL", LARGE_BYTES, cellGcmSegmentGetUsedBytes(&s), polls, SEG_LABEL_RETIRE_L,
           (unsigned)label_value(SEG_LABEL_RETIRE_L), SEG_LABEL_PROBE, (unsigned)label_value(SEG_LABEL_PROBE));
    seg_step("large %s retired=%d polls=%u", ok ? "ok" : "FAIL", retired, polls);
    return ok;
}
#endif

int main(void)
{
    sys_addr_t host = 0, pool = 0;
    uint32_t pool_offset = 0;
    CellGcmConfig config;
    CellGcmSegment seg[2];
    PhaseStats st = {.name = VARIANT};
    const char *outcome = "-";
    (void)seg; (void)config;

    seg_crumb_open("hello-ppu-gcm-call-return-" VARIANT);
    if (gcm_host_buffer_alloc(HOST_BYTES, &host) != 0 ||
        cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) || !seg_display_init(&disp)) {
        puts("GCM_SEG_FAIL initialization"); seg_step("FAIL initialization"); return 1;
    }
    CellGcmContextData *ring = CELL_GCM_CURRENT;
    if (cellSysutilRegisterCallback(0, on_sysutil, NULL)) { puts("GCM_SEG_FAIL sysutil"); return 1; }
    cellGcmGetConfiguration(&config);

    /* Main-memory pool: separate 1 MiB-page memory, mapped for the RSX.
     * Layout: overflow probe at 0, frame segments at 64 KiB, large at 192 KiB. */
    if (gcm_host_buffer_alloc(POOL_BYTES, &pool) != 0 ||
        cellGcmMapMainMemory((void *)(uintptr_t)pool, POOL_BYTES, &pool_offset) != CELL_OK) {
        puts("GCM_SEG_FAIL map main memory"); seg_step("FAIL map main memory"); return 1;
    }
    uint8_t *pool_bytes = (uint8_t *)(uintptr_t)pool;
    printf("GCM_SEG_DISPLAY variant=%s width=%u height=%u pool_io_offset=0x%08x\n",
           VARIANT, disp.width, disp.height, (unsigned)pool_offset);
    seg_step("init variant=%s width=%u height=%u pool_io_offset=0x%08x", VARIANT, disp.width,
             disp.height, (unsigned)pool_offset);
    for (unsigned k = 0; k < 2; ++k) *(volatile uint32_t *)cellGcmGetLabelAddress(progress_label[k]) = 0;

    if (!overflow_check(ring, (uint32_t *)pool_bytes)) { puts("GCM_SEG_FAIL overflow check"); return 1; }

#if defined(GCM_SEG_VARIANT_LARGE)
    if (!large_check(ring, (uint32_t *)(pool_bytes + 3 * SEG_BYTES))) { puts("GCM_SEG_FAIL large"); return 1; }
    outcome = "large-ok";
#elif defined(GCM_SEG_VARIANT_LOCAL)
    /* RSX local memory after the display buffers, 1 MiB aligned.  Each local
     * segment gets a decoy at the equal IO offset (inside the host buffer). */
    uint32_t local_off = (disp.local_used + 0xfffffu) & ~0xfffffu;
    if (local_off + 2u * SEG_BYTES > config.localSize) { puts("GCM_SEG_FAIL local space"); return 1; }
    for (unsigned k = 0; k < 2; ++k) {
        uint32_t off = local_off + k * SEG_BYTES;
        void *decoy = NULL;
        if (cellGcmIoOffsetToAddress(off, &decoy) != CELL_OK ||
            (uintptr_t)decoy < (uintptr_t)host + FIFO_BYTES ||
            (uintptr_t)decoy + SEG_BYTES > (uintptr_t)host + HOST_BYTES) {
            printf("GCM_SEG_FAIL no decoy room at io offset 0x%08x\n", (unsigned)off); return 1;
        }
        build_decoy((uint32_t *)decoy, SEG_BYTES, k);
        void *mem = (void *)((uintptr_t)config.localAddress + local_off + k * SEG_BYTES);
        if (cellGcmSegmentInit(&seg[k], mem, SEG_BYTES, CELL_GCM_LOCATION_LOCAL, retire_label[k]) != CELL_OK) {
            puts("GCM_SEG_FAIL local segment init"); return 1;
        }
    }
    seg_step("local segments at local offset 0x%08x, decoys at the equal io offsets", (unsigned)local_off);
    if (!run_phase(ring, seg, 1, &st)) { puts("GCM_SEG_LOCAL outcome=LOCAL-STALLED"); return 1; }
    outcome = (st.decoy_hits == FRAMES && !st.mismatches) ? "LOCAL-IO-ALIAS"
            : (!st.decoy_hits && !st.mismatches) ? "LOCAL-OK" : "LOCAL-MIXED";
    printf("GCM_SEG_PHASE name=local frames=%u local_offset=0x%08x waits_polled=%u max_polls=%u "
           "bytes_per_segment=%u progress_mismatches=%u decoy=%u outcome=%s\n", st.frames,
           (unsigned)local_off, st.waits_polled, st.max_polls, st.used_bytes, st.mismatches,
           st.decoy_hits, outcome);
    if (!strcmp(outcome, "LOCAL-MIXED")) { puts("GCM_SEG_FAIL local outcome mixed"); return 1; }
#elif defined(GCM_SEG_VARIANT_JUMP)
    CellGcmSegment sub[2];
    for (unsigned k = 0; k < 2; ++k)
        if (cellGcmSegmentInit(&seg[k], pool_bytes + (1 + k) * SEG_BYTES, SEG_BYTES,
                               CELL_GCM_LOCATION_MAIN, retire_label[k]) != CELL_OK ||
            cellGcmSegmentInit(&sub[k], pool_bytes + (3 + k) * SEG_BYTES, SEG_BYTES,
                               CELL_GCM_LOCATION_MAIN, SEG_LABEL_PROBE) != CELL_OK) {
            puts("GCM_SEG_FAIL jump segment init"); return 1;
        }
    if (!run_jump_phase(ring, seg, sub, &st)) return 1;
    printf("GCM_SEG_PHASE name=jump frames=%u calls=%u jumps=%u waits_polled=%u max_polls=%u bytes_per_frame=%u "
           "progress_mismatches=%u\n", st.frames, seg[0].calls + seg[1].calls, st.frames,
           st.waits_polled, st.max_polls, st.used_bytes, st.mismatches);
    if (st.mismatches || st.frames != FRAMES) { puts("GCM_SEG_FAIL progress"); return 1; }
    outcome = "jump-ok";
#else
    for (unsigned k = 0; k < 2; ++k)
        if (cellGcmSegmentInit(&seg[k], pool_bytes + (1 + k) * SEG_BYTES, SEG_BYTES,
                               CELL_GCM_LOCATION_MAIN, retire_label[k]) != CELL_OK) {
            puts("GCM_SEG_FAIL main segment init"); return 1;
        }
    if (!run_phase(ring, seg, 0, &st)) return 1;
    printf("GCM_SEG_PHASE name=main frames=%u calls=%u waits_polled=%u max_polls=%u bytes_per_segment=%u "
           "progress_mismatches=%u decoy=%u\n", st.frames, seg[0].calls + seg[1].calls,
           st.waits_polled, st.max_polls, st.used_bytes, st.mismatches, st.decoy_hits);
    if (st.mismatches || st.decoy_hits) { puts("GCM_SEG_FAIL progress"); return 1; }
    outcome = "main-ok";
#endif
    cellSysutilUnregisterCallback(0);
    if (quit_requested) { puts("GCM_SEG_QUIT"); return 0; }
    printf("GCM_SEG_PASS variant=%s frames=%u outcome=%s overflow=refused\n", VARIANT, st.frames, outcome);
    seg_step("PASS variant=%s outcome=%s", VARIANT, outcome);
    seg_crumb_close();
    return 0;
}
