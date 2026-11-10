/*
 * Host test for GCM command segments: cellGcmSetupContextData (B1) and
 * the label-fenced segment helper in sdk/libgcm_cmd/src/gcm_segment.c.
 *
 * Built by gcm-segment-test.sh from the real sources: the rsx emitters
 * (rsx_legacy/commands.c), the cellGcm link wrappers (gcm_rsx_wrappers.c,
 * which holds cellGcmSetupContextData) and gcm_segment.c, against small
 * stubs for the address, label and sleep services.  A model RSX front end
 * runs the ring: it follows CALL / RETURN / JUMP with one return slot and
 * performs the label writes, so retirement is driven by the commands the
 * helper really emitted.
 *
 * Checks:
 *   emitters      eight existing emitters write into a segment context the
 *                 exact words they write into a plain buffer, then RETURN;
 *   overflow      a full segment refuses the next command (no wrap, no
 *                 write past the buffer), counts the refused words, End
 *                 reports OVERFLOW and Call refuses; an exact fit succeeds;
 *   lifecycle     Begin on an in-flight segment is refused; the bounded
 *                 wait returns TIMEOUT after exactly maxPolls sleeps;
 *   double-buffer two segments called alternately over 64 frames retire
 *                 through their back-end labels and are reused only then;
 *   nested        a segment that CALLs is flagged by the model (the model
 *                 has one return slot; the console/emulator outcome is
 *                 measured by hello-ppu-gcm-nested-call, not here).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/commands.h>
#include <rsx/nv40.h>
#include <cell/gcm/gcm_segment.h>

typedef gcmContextCallback CellGcmContextCallback;
void cellGcmSetupContextData(gcmContextData *context, const uint32_t *addr,
                             const uint32_t size, CellGcmContextCallback callback);

/* ---- stubs: one RSX-visible arena, 256 labels, a counting sleep ---- */

#define ARENA_WORDS (1u << 16)
#define ARENA_BASE_OFFSET 0x00100000u
static uint32_t arena[ARENA_WORDS] __attribute__((aligned(128)));
static uint32_t labels[256];
static unsigned sleeps;
static void (*on_sleep)(void);

s32 gcmAddressToOffset(const void *address, u32 *offset)
{
    uintptr_t a = (uintptr_t)address, b = (uintptr_t)arena;
    if (a < b || a >= b + sizeof(arena)) return -1;
    *offset = ARENA_BASE_OFFSET + (u32)(a - b);
    return 0;
}

u32 *gcmGetLabelAddress(const u8 index) { return &labels[index]; }

void ps3tc_gcm_segment_test_sleep(uint32_t microseconds)
{
    (void)microseconds;
    ++sleeps;
    if (on_sleep) on_sleep();
}

static uint32_t *at_offset(uint32_t offset)
{
    if (offset < ARENA_BASE_OFFSET || offset - ARENA_BASE_OFFSET >= sizeof(arena)) return NULL;
    return &arena[(offset - ARENA_BASE_OFFSET) / 4u];
}

/* ---- model front end ---- */

typedef struct {
    uint32_t get, put, ret;         /* offsets; ret == 0: no return pending */
    unsigned nested_calls, bad;     /* CALL while a return is pending       */
    uint32_t sem_offset;            /* last SEMAPHORE_OFFSET (either class) */
    unsigned label_writes;
} Model;
static Model model;

static void model_method(uint32_t method, uint32_t value)
{
    if (method == NV40TCL_SEMAPHORE_OFFSET || method == NV406ETCL_SEMAPHORE_OFFSET) {
        model.sem_offset = value;
    } else if (method == NV40TCL_SEMAPHORE_BACKENDWRITE_RELEASE) {
        /* The emitter swaps bytes 0 and 2 for the back end; the stored
         * label is the value the caller passed. */
        labels[(model.sem_offset / 0x10u) & 0xff] =
            (value & 0xff00ff00u) | ((value >> 16) & 0xffu) | ((value & 0xffu) << 16);
        ++model.label_writes;
    } else if (method == NV406ETCL_SEMAPHORE_RELEASE) {
        labels[(model.sem_offset / 0x10u) & 0xff] = value;
        ++model.label_writes;
    }
}

/* Run from get to put; at most `budget` words (a stuck model fails). */
static int model_run(unsigned budget)
{
    while (model.get != model.put) {
        uint32_t *p = at_offset(model.get), w;
        if (!p || !budget--) { model.bad++; return 0; }
        w = *p;
        if ((w & 0xe0000003u) == 0x20000000u) { model.get = w & 0x1ffffffcu; continue; }
        if ((w & 3u) == 2u) {
            if (model.ret) { model.nested_calls++; model.bad++; return 0; }
            model.ret = model.get + 4u;
            model.get = w & 0x1ffffffcu;
            continue;
        }
        if ((w & 0xffff0003u) == 0x00020000u) {
            if (!model.ret) { model.bad++; return 0; }
            model.get = model.ret; model.ret = 0;
            continue;
        }
        {
            uint32_t count = (w >> 18) & 0x7ffu, method = w & 0x1ffcu, i;
            int ni = (w & 0x40000000u) != 0;
            for (i = 0; i < count; ++i) {
                uint32_t *v = at_offset(model.get + 4u + 4u * i);
                if (!v) { model.bad++; return 0; }
                model_method(ni ? method : method + 4u * i, *v);
            }
            model.get += 4u + 4u * count;
        }
    }
    return 1;
}

/* ---- the ring: a plain context in the arena, no callback ---- */

#define RING_WORDS 8192u
static gcmContextData ring;
static uint32_t *ring_mem;

static void ring_reset(void)
{
    uint32_t off;
    ring_mem = &arena[ARENA_WORDS - RING_WORDS];
    ring.begin = ring.current = ring_mem;
    ring.end = ring_mem + RING_WORDS - 1u;
    ring.callback = NULL;
    gcmAddressToOffset(ring_mem, &off);
    memset(&model, 0, sizeof(model));
    model.get = model.put = off;
}

static void ring_flush(void) { gcmAddressToOffset(ring.current, &model.put); }

static int failures;
#define CHECK(name, cond) do { if (!(cond)) { printf("  FAIL %s: %s (line %d)\n", name, #cond, __LINE__); ++failures; } } while (0)

/* ---- emitters ---- */

static void emit_set(gcmContextData *c)
{
    static const f32 scale[4] = {640.0f, -360.0f, 0.5f, 0.0f};
    static const f32 offs[4] = {640.0f, 360.0f, 0.5f, 0.0f};
    rsxSetClearColor(c, 0xff2040c0u);
    rsxSetScissor(c, 16, 32, 320, 200);
    rsxClearSurface(c, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    rsxSetViewport(c, 0, 0, 1280, 720, 0.0f, 1.0f, scale, offs);
    rsxSetBlendFunc(c, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ZERO);
    rsxSetWriteCommandLabel(c, 201, 0x12345678u);
    rsxSetDepthTestEnable(c, GCM_TRUE);
    rsxDrawVertexArray(c, GCM_TYPE_TRIANGLES, 0, 3);
}

static void test_emitters(void)
{
    static uint32_t plain[4096];
    gcmContextData p = { plain, plain + 4095, plain, NULL };
    CellGcmSegment s;
    gcmContextData *c;
    size_t words;

    puts("emitters:");
    emit_set(&p);
    words = (size_t)(p.current - plain);
    CHECK("init", cellGcmSegmentInit(&s, &arena[0], 4096, GCM_LOCATION_CELL, 200) == CELL_OK);
    c = cellGcmSegmentBegin(&s);
    CHECK("begin", c != NULL);
    if (!c) return;
    emit_set(c);
    CHECK("end", cellGcmSegmentEnd(&s) == CELL_OK);
    CHECK("words", memcmp(&arena[0], plain, words * 4u) == 0);
    CHECK("return", arena[words] == 0x00020000u);
    CHECK("used", cellGcmSegmentGetUsedBytes(&s) == (words + 1u) * 4u);
    printf("  %zu emitted words + RETURN identical to a plain buffer\n", words);
}

static void test_overflow(void)
{
    CellGcmSegment s;
    gcmContextData *c;
    unsigned i;

    puts("overflow:");
    memset(arena, 0xa5, 256);                       /* canary past the segment */
    CHECK("init", cellGcmSegmentInit(&s, &arena[0], 128, GCM_LOCATION_CELL, 200) == CELL_OK);
    c = cellGcmSegmentBegin(&s);
    if (!c) { CHECK("begin", 0); return; }
    for (i = 0; i < 20; ++i) rsxSetClearColor(c, 0xff000000u | i);
    CHECK("written", c->current == &arena[30]);     /* 15 commands of 2 words */
    for (i = 0; i < 15; ++i) CHECK("prefix", arena[2 * i + 1] == (0xff000000u | i));
    CHECK("refused", cellGcmSegmentGetOverflowWords(&s) == 10u);
    CHECK("end", cellGcmSegmentEnd(&s) == CELL_GCM_SEGMENT_ERROR_OVERFLOW);
    CHECK("canary", arena[32] == 0xa5a5a5a5u && arena[63] == 0xa5a5a5a5u);
    ring_reset();
    CHECK("call refused", cellGcmSegmentCall(&ring, &s) == CELL_GCM_SEGMENT_ERROR_OVERFLOW);
    CHECK("ring untouched", ring.current == ring.begin);
    printf("  32-word segment: 15 commands kept, 10 words refused, End=OVERFLOW, Call refused\n");

    /* Exactly 31 words (one left for RETURN): 15 x 2 + a 1-word NOP. */
    c = cellGcmSegmentBegin(&s);
    if (!c) { CHECK("rebegin", 0); return; }
    for (i = 0; i < 15; ++i) rsxSetClearColor(c, i);
    rsxSetNopCommand(c, 1);
    CHECK("exact end", cellGcmSegmentEnd(&s) == CELL_OK);
    CHECK("exact used", cellGcmSegmentGetUsedBytes(&s) == 128u);
    CHECK("exact return", arena[31] == 0x00020000u && arena[32] == 0xa5a5a5a5u);
    printf("  exact fit: 31 words + RETURN = 128 bytes, End=OK\n");
}

static void test_lifecycle(void)
{
    CellGcmSegment s;
    uint32_t polls = 99;

    puts("lifecycle:");
    ring_reset();
    CHECK("init", cellGcmSegmentInit(&s, &arena[256], 256, GCM_LOCATION_CELL, 202) == CELL_OK);
    CHECK("not open yet", cellGcmSegmentEnd(&s) == CELL_GCM_SEGMENT_ERROR_STATE);
    CHECK("begin", cellGcmSegmentBegin(&s) != NULL);
    CHECK("double begin", cellGcmSegmentBegin(&s) == NULL);
    CHECK("call open", cellGcmSegmentCall(&ring, &s) == CELL_GCM_SEGMENT_ERROR_STATE);
    CHECK("end", cellGcmSegmentEnd(&s) == CELL_OK);
    CHECK("call", cellGcmSegmentCall(&ring, &s) == CELL_OK);
    CHECK("busy", cellGcmSegmentBegin(&s) == NULL);
    sleeps = 0; on_sleep = NULL;
    CHECK("timeout", cellGcmSegmentWaitRetired(&s, 7, 100, &polls) == CELL_GCM_SEGMENT_ERROR_TIMEOUT);
    CHECK("bounded", sleeps == 7 && polls == 7);
    CHECK("still busy", cellGcmSegmentBegin(&s) == NULL);
    ring_flush();
    CHECK("run", model_run(100000));
    CHECK("label", labels[202] == 1u);
    CHECK("retired", cellGcmSegmentWaitRetired(&s, 7, 100, &polls) == CELL_OK && polls == 0);
    CHECK("reuse", cellGcmSegmentBegin(&s) != NULL);
    printf("  in-flight Begin refused; wait(7) = TIMEOUT after %u sleeps; retired after the model ran\n", sleeps);
}

/* The model RSX lags one frame: each sleep lets it run to the current PUT. */
static void run_on_sleep(void) { model_run(1000000); }

static void test_double_buffer(void)
{
    CellGcmSegment s[2];
    unsigned f, waited = 0, polls_total = 0;

    puts("double-buffer:");
    ring_reset();
    CHECK("init0", cellGcmSegmentInit(&s[0], &arena[1024], 1024, GCM_LOCATION_CELL, 203) == CELL_OK);
    CHECK("init1", cellGcmSegmentInit(&s[1], &arena[2048], 1024, GCM_LOCATION_CELL, 204) == CELL_OK);
    on_sleep = run_on_sleep;
    for (f = 0; f < 64; ++f) {
        CellGcmSegment *seg = &s[f & 1u];
        uint32_t polls = 0;
        gcmContextData *c;
        if (ring.current + 64 > ring.end) {          /* keep the model ring linear */
            CHECK("drain", model_run(1000000));
            ring.current = ring.begin;
            gcmAddressToOffset(ring.begin, &model.get);
            model.put = model.get;
        }
        CHECK("wait", cellGcmSegmentWaitRetired(seg, 4, 10, &polls) == CELL_OK);
        polls_total += polls; waited += polls != 0;
        c = cellGcmSegmentBegin(seg);
        if (!c) { CHECK("begin", 0); break; }
        rsxSetClearColor(c, f);
        rsxSetWriteCommandLabel(c, 205, f);
        CHECK("end", cellGcmSegmentEnd(seg) == CELL_OK);
        CHECK("call", cellGcmSegmentCall(&ring, seg) == CELL_OK);
        ring_flush();
    }
    CHECK("final run", model_run(1000000));
    on_sleep = NULL;
    CHECK("serials", labels[203] == 32u && labels[204] == 32u && s[0].calls == 32u);
    CHECK("segment label", labels[205] == 63u);
    CHECK("model clean", model.bad == 0 && model.ret == 0);
    printf("  64 frames, 2 segments, %u waits polled (%u polls), labels %u/%u, model errors %u\n",
           waited, polls_total, labels[203], labels[204], model.bad);
}

static void test_nested(void)
{
    CellGcmSegment outer, inner;
    gcmContextData *c;

    puts("nested:");
    ring_reset();
    CHECK("init", cellGcmSegmentInit(&outer, &arena[4096], 256, GCM_LOCATION_CELL, 206) == CELL_OK);
    CHECK("init", cellGcmSegmentInit(&inner, &arena[4160], 256, GCM_LOCATION_CELL, 207) == CELL_OK);
    c = cellGcmSegmentBegin(&inner);
    if (!c) { CHECK("begin", 0); return; }
    rsxSetWriteCommandLabel(c, 208, 2);
    cellGcmSegmentEnd(&inner);
    c = cellGcmSegmentBegin(&outer);
    if (!c) { CHECK("begin", 0); return; }
    rsxSetWriteCommandLabel(c, 208, 1);
    rsxSetCallCommand(c, inner.offset);
    rsxSetWriteCommandLabel(c, 208, 3);
    cellGcmSegmentEnd(&outer);
    labels[208] = 0;
    CHECK("call", cellGcmSegmentCall(&ring, &outer) == CELL_OK);
    ring_flush();
    CHECK("model refuses", !model_run(100000) && model.nested_calls == 1);
    CHECK("reached before", labels[208] == 1u);
    printf("  one-slot model: nested CALL flagged after label 1\n");
}

/* A segment written by another writer (the SPU path). */
static void test_external(void)
{
    CellGcmSegment s;
    uint32_t *m;

    puts("external:");
    ring_reset();
    CHECK("init", cellGcmSegmentInit(&s, &arena[8192], 256, GCM_LOCATION_CELL, 209) == CELL_OK);
    m = cellGcmSegmentBeginExternal(&s);
    CHECK("claim", m == &arena[8192]);
    if (!m) return;
    CHECK("no ppu begin", cellGcmSegmentBegin(&s) == NULL);
    CHECK("no double claim", cellGcmSegmentBeginExternal(&s) == NULL);
    CHECK("no internal end", cellGcmSegmentEnd(&s) != CELL_OK);
    /* SEMAPHORE_OFFSET = label 210, SEMAPHORE_RELEASE 0x77, RETURN. */
    m[0] = 0x00040064u; m[1] = 0x10u * 210u; m[2] = 0x0004006cu; m[3] = 0x77u;
    m[4] = 0x00020000u;
    CHECK("bad size", cellGcmSegmentEndExternal(&s, 18) == CELL_GCM_SEGMENT_ERROR_INVALID_VALUE);
    CHECK("no return", cellGcmSegmentEndExternal(&s, 16) == CELL_GCM_SEGMENT_ERROR_INVALID_VALUE);
    CHECK("too big", cellGcmSegmentEndExternal(&s, 260) == CELL_GCM_SEGMENT_ERROR_INVALID_VALUE);
    CHECK("end", cellGcmSegmentEndExternal(&s, 20) == CELL_OK);
    CHECK("used", cellGcmSegmentGetUsedBytes(&s) == 20u);
    labels[210] = 0;
    CHECK("call", cellGcmSegmentCall(&ring, &s) == CELL_OK);
    ring_flush();
    CHECK("run", model_run(10000));
    CHECK("ran", labels[210] == 0x77u && labels[209] == 1u);
    CHECK("reclaim", cellGcmSegmentBeginExternal(&s) != NULL);
    printf("  claimed, bad sizes and a missing RETURN refused, ran, reclaimed after retirement\n");
}

int main(void)
{
    test_emitters();
    test_overflow();
    test_lifecycle();
    test_double_buffer();
    test_nested();
    test_external();
    if (failures) { printf("gcm-segment: %d check(s) FAILED\n", failures); return 1; }
    puts("gcm-segment: all checks passed");
    return 0;
}
