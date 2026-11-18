/* GCM command segments: each frame's drawing is built into a small command
 * segment and run from the ring with CALL; the segment ends in RETURN.
 * Two segments are reused in turn, each only after the back-end label
 * written after its CALL has come back.  Every wait is bounded by a poll
 * count and names the label it waits for.  No shaders, no input devices.
 *
 * Variants (CMakeLists.txt builds one SELF per variant):
 *   default  overflow refusal (no CALL) + 120 frames from mapped main memory
 *   -local   overflow refusal + cellGcmSegmentInit on RSX local memory, which
 *            must be refused with the segment and its label untouched: CALL
 *            offsets are IO offsets (GCM_SEG_VARIANT_LOCAL)
 *   -large   overflow refusal + one 1 MiB segment (GCM_SEG_VARIANT_LARGE)
 *   -jump    overflow refusal + 120 frames from mapped main memory, each a
 *            CALLed segment that JUMPs to a second sub-segment, which RETURNs
 *            (GCM_SEG_VARIANT_JUMP)
 *   -ring    600 owner-buffer clears per frame for the AUTO report ring
 *            (GCM_SEG_VARIANT_RING): its own attach selects the ring, or, in a
 *            PS3_PROF_AUTO_RING=ON build, the automatic attach already did and
 *            it only prints the build option; built with -DGCM_SEG_RING_PROBE, the first
 *            3 flips print the flip words and IO offset 0 (GCM_SEG_PROBE)
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
#include <ps3dk/prof.h>   /* capture windows: PS3DK_PROF_CAPTURE_WHOLE_RUN (inert unless profiled) */
#include <ps3dk/prof_auto.h>

#if defined(GCM_SEG_VARIANT_LOCAL)
#define VARIANT "local"
#elif defined(GCM_SEG_VARIANT_LARGE)
#define VARIANT "large"
#elif defined(GCM_SEG_VARIANT_JUMP)
#define VARIANT "jump"
#elif defined(GCM_SEG_VARIANT_RING)
#define VARIANT "ring"
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
 * phase 0 (main memory): green stripe, one bar; phase 1: blue, two. */
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

#ifdef GCM_SEG_VARIANT_LOCAL
/* RSX local memory after the display buffers, 1 MiB aligned: Init must
 * refuse it and leave the segment and its label as they were. */
static int local_refusal_check(const CellGcmConfig *config)
{
    CellGcmSegment s, before;
    volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(SEG_LABEL_RETIRE_L);
    uint32_t local_off = (disp.local_used + 0xfffffu) & ~0xfffffu;
    if (local_off + SEG_BYTES > config->localSize) { puts("GCM_SEG_FAIL local space"); return 0; }
    void *mem = (void *)((uintptr_t)config->localAddress + local_off);
    memset(&s, 0x5c, sizeof(s));
    memcpy(&before, &s, sizeof(s));
    *label = 0x600dcafeu;
    int32_t rc = cellGcmSegmentInit(&s, mem, SEG_BYTES, CELL_GCM_LOCATION_LOCAL, SEG_LABEL_RETIRE_L);
    int untouched = !memcmp(&s, &before, sizeof(s)) && *label == 0x600dcafeu;
    int ok = rc == CELL_GCM_SEGMENT_ERROR_INVALID_VALUE && untouched;
    printf("GCM_SEG_LOCAL %s local_offset=0x%08x init=%08x untouched=%s\n", ok ? "refused" : "FAIL",
           (unsigned)local_off, (unsigned)rc, untouched ? "yes" : "no");
    seg_step("local init %s rc=%08x untouched=%d", ok ? "refused" : "FAIL", (unsigned)rc, untouched);
    *label = 0;
    return ok;
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

#if defined(GCM_SEG_VARIANT_RING)
/* -ring: owner-buffer clears only, RING_DRAWS per frame (above the 504-pair local ceiling), timed through the AUTO
 * report ring when the SDK was built with it. The attach precedes every device write of the application. */
enum { RING_FRAMES = 30, RING_DRAWS = 600 };
#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
#include <ps3dk/prof_auto.h>
/* Defined by the generated unit of a PS3_PROF_AUTO_RING=ON build: the automatic attach already selected the ring. */
extern const ps3dk_prof_auto_ring_options ps3dk_prof_automatic_ring_options __attribute__((weak));
#endif
static void ring_attach(void)
{
#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
    ps3dk_prof_auto_options o = PS3DK_PROF_AUTO_OPTIONS_INIT;
    uint32_t instance = 0, domain = 0;
    if (&ps3dk_prof_automatic_ring_options) {
        const ps3dk_prof_auto_ring_options *r = &ps3dk_prof_automatic_ring_options;
        printf("GCM_SEG_RING attach from the build option: blocks=%u bytes=%u records=%u\n", (unsigned)r->ring_blocks,
               (unsigned)r->ring_bytes, (unsigned)r->ring_records);
        return;
    }
    o.ring_blocks = 2;   /* 16 MiB of MAIN extents, 16,384 records, label 245: the defaults */
    int rc = ps3dk_prof_auto_attach((ps3dk_prof_gcm_ctx *)(void *)CELL_GCM_CURRENT, &o, &instance, &domain);
    printf("GCM_SEG_RING attach rc=%d instance=%u domain=%u\n", rc, (unsigned)instance, (unsigned)domain);
#else
    puts("GCM_SEG_RING attach skipped (not a PS3_PROF_AUTO build)");
#endif
}
#ifdef GCM_SEG_RING_PROBE
/* Flip probe (first PROBE_FLIPS frames): the words cellGcmSetFlip writes into the owner buffer, every CALL among
 * them with the word after it, and the first PROBE_WORDS words at IO offset 0 before SetFlip, after it and after
 * the flip completed. GCM_SEG_PROBE lines; an all-zero row of 8 words is not printed. Off in the default -ring
 * build, so a timing run prints nothing extra. */
enum { PROBE_FLIPS = 3, PROBE_WORDS = 1024, PROBE_SPAN = 160 };
static uint32_t probe_pre[PROBE_WORDS], probe_post[PROBE_WORDS], probe_first[PROBE_WORDS];
static void probe_copy(uint32_t *to, const void *io0)
{
    const volatile uint32_t *w = (const volatile uint32_t *)io0;
    for (unsigned i = 0; i < PROBE_WORDS; ++i) to[i] = w[i];
}
static void probe_rows(const char *tag, unsigned flip, uint32_t base, const uint32_t *w, unsigned n, int skip_zero)
{
    for (unsigned i = 0; i < n; i += 8) {
        unsigned k = n - i < 8 ? n - i : 8, nz = 0;
        for (unsigned j = 0; j < k; ++j) nz |= w[i + j] != 0;
        if (skip_zero && !nz) continue;
        printf("GCM_SEG_PROBE %s flip=%u +0x%04x:", tag, flip, (unsigned)(base + 4u * i));
        for (unsigned j = 0; j < k; ++j) printf(" %08x", (unsigned)w[i + j]);
        putchar('\n');
    }
}
static void probe_diff(const char *tag, unsigned flip, const uint32_t *a, const uint32_t *b)
{
    unsigned n = 0;
    for (unsigned i = 0; i < PROBE_WORDS; ++i)
        if (a[i] != b[i]) {
            if (n < 48) printf("GCM_SEG_PROBE %s flip=%u +0x%04x %08x -> %08x\n", tag, flip, 4u * i,
                               (unsigned)a[i], (unsigned)b[i]);
            ++n;
        }
    printf("GCM_SEG_PROBE %s flip=%u changed_words=%u\n", tag, flip, n);
}
static int ring_flip_probe(CellGcmContextData *ctx, SegDisplay *d, unsigned flip, uint32_t host)
{
    void *io0 = NULL;
    uint32_t begin_io = 0, *before = ctx->current, *begin = ctx->begin;
    int32_t rc0 = cellGcmIoOffsetToAddress(0, &io0), rcb = cellGcmAddressToOffset(begin, &begin_io);
    if (rc0 == CELL_OK && io0) probe_copy(probe_pre, io0);
    cellGcmResetFlipStatus();
    if (cellGcmSetFlip(ctx, (uint8_t)d->buffer_id)) return 0;
    uint32_t *after = ctx->current;
    if (rc0 == CELL_OK && io0) probe_copy(probe_post, io0);
    int same = ctx->begin == begin && after >= before;
    unsigned n = same ? (unsigned)(after - before) : 0;
    printf("GCM_SEG_PROBE flip=%u io0 rc=%08x ea=0x%08x host=0x%08x begin=0x%08x begin_io rc=%08x off=0x%08x "
           "end=+0x%x before=+0x%x after=+0x%x same_begin=%d words=%u\n", flip, (unsigned)rc0,
           (unsigned)(uintptr_t)io0, (unsigned)host, (unsigned)(uintptr_t)begin, (unsigned)rcb, (unsigned)begin_io,
           (unsigned)(4u * (ctx->end - ctx->begin)), (unsigned)(4u * (before - begin)),
           (unsigned)(4u * (after - ctx->begin)), same, n);
    for (unsigned i = 0; i < n && i < PROBE_SPAN; ++i)
        if ((before[i] & 3u) == 2u)
            printf("GCM_SEG_PROBE flip=%u call at=+0x%x word=0x%08x target_io=0x%08x next=0x%08x\n", flip,
                   (unsigned)(4u * (before + i - begin)), (unsigned)before[i], (unsigned)(before[i] & ~3u),
                   i + 1 < n ? (unsigned)before[i + 1] : 0xffffffffu);
    probe_rows("owner", flip, (uint32_t)(4u * (before - begin)), before, n < PROBE_SPAN ? n : PROBE_SPAN, 0);
    if (rc0 == CELL_OK && io0) {
        if (flip == 0) {
            probe_rows("io0-pre", flip, 0, probe_pre, PROBE_WORDS, 1);
            probe_rows("io0-post", flip, 0, probe_post, PROBE_WORDS, 1);
            for (unsigned i = 0; i < PROBE_WORDS; ++i) probe_first[i] = probe_post[i];
        }
        probe_diff("io0-setflip", flip, probe_pre, probe_post);
        if (flip) probe_diff("io0-vs-flip0", flip, probe_first, probe_post);
    }
    cellGcmFlush(ctx);
    if (cellGcmWaitFlipComplete(SEG_FLIP_POLLS * 200u) != CELL_OK) return 0;
    if (rc0 == CELL_OK && io0) { probe_copy(probe_pre, io0); probe_diff("io0-completed", flip, probe_post, probe_pre); }
    d->buffer_id ^= 1u;
    return 1;
}
#endif
static int run_ring_phase(CellGcmContextData *ring, PhaseStats *st, uint32_t host)
{
    for (unsigned frame = 0; frame < RING_FRAMES && !quit_requested; ++frame) {
        seg_display_target(ring, &disp);
        for (unsigned i = 0; i < RING_DRAWS; ++i) {
            cellGcmSetClearColor(ring, 0xff000000u | ((frame * 7u + i) & 0xffu) << 8);
            cellGcmSetClearSurface(ring, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
        }
#ifdef GCM_SEG_RING_PROBE
        if (frame < PROBE_FLIPS ? !ring_flip_probe(ring, &disp, frame, host)
                                : !seg_display_flip(ring, &disp, SEG_FLIP_POLLS)) { puts("GCM_SEG_FAIL ring flip"); return 0; }
#else
        (void)host;
        if (!seg_display_flip(ring, &disp, SEG_FLIP_POLLS)) { puts("GCM_SEG_FAIL ring flip"); return 0; }
#endif
        st->frames++;
        cellSysutilCheckCallback();
    }
    printf("GCM_SEG_PHASE name=ring frames=%u draws_per_frame=%u\n", st->frames, (unsigned)RING_DRAWS);
    return 1;
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
    /* a profiled build (PS3DK_PROF or PS3_PROF_AUTO) records nothing until a window opens: record the whole run */
    (void)PS3DK_PROF_CAPTURE_WHOLE_RUN();

    seg_crumb_open("hello-ppu-gcm-call-return-" VARIANT);
    if (gcm_host_buffer_alloc(HOST_BYTES, &host) != 0 ||
        cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host)
#if defined(GCM_SEG_VARIANT_RING)
        || (ring_attach(), 0)
#endif
        || !seg_display_init(&disp)) {
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
    if (!local_refusal_check(&config)) { puts("GCM_SEG_FAIL local refusal"); return 1; }
    outcome = "local-refused";
#elif defined(GCM_SEG_VARIANT_RING)
    if (!run_ring_phase(ring, &st, (uint32_t)host)) return 1;
    outcome = "ring-ok";
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
