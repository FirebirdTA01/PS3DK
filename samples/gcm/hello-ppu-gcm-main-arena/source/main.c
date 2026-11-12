/* hello-ppu-gcm-main-arena: vertices, indices and a texture drawn from a cellGcmMapMainMemory'd arena
 * (CELL_GCM_LOCATION_MAIN), with the PPU rewriting arena content between phases.
 *
 * Unchecked warm-up frames run first until one reads back right (an emulator that compiles shaders asynchronously
 * skips the first draws).  Then three phases, 60 frames each, every frame checked by readback:
 *   1  texture colour A, quad over the readback window             -> the whole window is A
 *   2  PPU rewrites the texture to B (after frame completion)       -> the whole window is B
 *   3  PPU rewrites the vertices: quad covers only the left half    -> left half B, right half the clear colour
 * The readback is an RSX image transfer (LOCAL_TO_MAIN) of a 128x128 window at the screen centre into the arena,
 * completed by a back-end label (application label 100) that the PPU polls with cellGcmIsLabelPassed.
 *
 * Output: GCM_ARENA_* lines; the last is GCM_ARENA_OK frames=180 or GCM_ARENA_FAIL <reason>. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <sys/process.h>

#include "../../common/gcm_arena_common.h"
#include "arena_fp_fpo.h"
#include "arena_vp_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20, ARENA_BYTES = 1 << 20 };
enum { FRAMES_PER_PHASE = 60, PHASES = 3, TEX_W = 64, TEX_H = 64, WIN = 128 };
#define ARENA_LABEL 100u /* application range (CELL_GCM_LABEL_APP_FIRST..LAST) */
#define ARENA_VBO 0x00000u
#define ARENA_IBO 0x01000u
#define ARENA_TEX 0x10000u
#define ARENA_READBACK 0x80000u
#define COLOR_A 0xff2080e0u
#define COLOR_B 0xffe0a020u
#define COLOR_CLEAR 0xff101820u
#define POLLS 200000u /* x 10 us: a 2 s watchdog per wait, reported as a failure */
#define WARMUP_MAX 600u /* unchecked warm-up frames at most (about 10 s at 60 Hz) */

_Static_assert(ARENA_LABEL >= CELL_GCM_LABEL_APP_FIRST && ARENA_LABEL <= CELL_GCM_LABEL_APP_LAST, "app label");
_Static_assert(ARENA_TEX % 128 == 0 && ARENA_VBO % 128 == 0 && ARENA_IBO % 128 == 0, "128-byte alignment");
_Static_assert(ARENA_READBACK + WIN * WIN * 4 <= ARENA_BYTES, "readback fits the arena");

static volatile int quit;
static void on_sysutil(uint64_t status, uint64_t param, void *data)
{
    (void)param, (void)data;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        quit = 1;
}

static int wait_label(uint32_t serial)
{
    for (uint32_t i = 0; i < POLLS; ++i) {
        if (cellGcmIsLabelPassed(ARENA_LABEL, serial))
            return 0;
        usleep(10);
    }
    return -1;
}

static int present(CellGcmContextData *ctx, uint8_t id)
{
    cellGcmResetFlipStatus();
    if (cellGcmSetFlip(ctx, id))
        return -1;
    cellGcmFlush(ctx);
    for (uint32_t i = 0; i < POLLS; ++i) {
        if (cellGcmGetFlipStatus() == 0)
            return 0;
        cellSysutilCheckCallback();
        usleep(10);
    }
    return -1;
}

/* the readback window, sampled at a 5x5 grid inside each half */
static int check_window(const uint32_t *px, uint32_t left, uint32_t right, uint32_t *bad)
{
    for (unsigned gy = 0; gy < 5; ++gy)
        for (unsigned gx = 0; gx < 10; ++gx) {
            unsigned x = 6 + gx * 12 + (gx >= 5 ? 4 : 0), y = 16 + gy * 24;
            uint32_t want = (x < WIN / 2 ? left : right) & 0x00ffffffu, got = px[y * WIN + x] & 0x00ffffffu;
            if (got != want) {
                *bad = got | ((uint32_t)(x << 24));
                return -1;
            }
        }
    return 0;
}

int main(void)
{
    arena_display d;
    arena_program prog;
    sys_addr_t host = 0, arena_ea = 0;
    uint32_t arena_off = 0, misaligned_off = 0;
    CellGcmContextData *ctx;

    printf("GCM_ARENA_BEGIN frames=%u\n", FRAMES_PER_PHASE * PHASES);
    if (gcm_host_buffer_alloc(HOST_BYTES, &host) || cellGcmInit(FIFO_BYTES, HOST_BYTES, (void *)(uintptr_t)host) ||
        arena_display_init(&d)) {
        puts("GCM_ARENA_FAIL init");
        return 1;
    }
    ctx = CELL_GCM_CURRENT;
    cellSysutilRegisterCallback(0, on_sysutil, NULL);

    /* the arena: the first of 2 MiB of LV2 1 MiB pages (1 MiB aligned), mapped for the RSX.  The second MiB is the
     * probe for a deliberately invalid request (4 KiB past the 1 MiB alignment, size not a 1 MiB multiple): its
     * result is an observation of this platform's check, not a pass condition. */
    if (gcm_host_buffer_alloc(2u * ARENA_BYTES, &arena_ea) || (arena_ea & (ARENA_BYTES - 1))) {
        puts("GCM_ARENA_FAIL arena allocation");
        return 1;
    }
    int32_t rc = cellGcmMapMainMemory((const void *)(uintptr_t)arena_ea, ARENA_BYTES, &arena_off);
    int32_t mis = cellGcmMapMainMemory((const void *)(uintptr_t)(arena_ea + ARENA_BYTES + 0x1000), 0x80000u,
                                       &misaligned_off);
    printf("GCM_ARENA_MAP ea=0x%08" PRIx64 " size=0x%x offset=0x%08x rc=0x%08x misaligned_rc=0x%08x\n",
           (uint64_t)arena_ea, ARENA_BYTES, arena_off, (unsigned)rc, (unsigned)mis);
    if (rc) {
        puts("GCM_ARENA_FAIL cellGcmMapMainMemory");
        return 1;
    }
    if (mis == 0)
        puts("GCM_ARENA_NOTE the misaligned map was accepted on this platform");
    uint8_t *arena = (uint8_t *)(uintptr_t)arena_ea;
    arena_vertex *vbo = (arena_vertex *)(arena + ARENA_VBO);
    uint16_t *ibo = (uint16_t *)(arena + ARENA_IBO);
    uint32_t *tex = (uint32_t *)(arena + ARENA_TEX);
    uint32_t *readback = (uint32_t *)(arena + ARENA_READBACK);

    if (arena_program_init(&d, &prog, arena_vp_vpo, arena_fp_fpo)) {
        puts("GCM_ARENA_FAIL program");
        return 1;
    }

    /* Phase 1 content.  Plain PPU stores: the flush that publishes the first draw orders them (sync, then PUT). */
    arena_quad(vbo, -0.5f, -0.5f, 0.5f, 0.5f);
    static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    memcpy(ibo, idx, sizeof idx);
    arena_fill(tex, TEX_W * TEX_H, COLOR_A);
    *(volatile uint32_t *)cellGcmGetLabelAddress(ARENA_LABEL) = 0u; /* stale boot content never matches */
    cellGcmCgUploadInternalConsts(ctx, prog.vp);

    uint32_t serial = 0, frames = 0, buffer = 0, warmup = 0;
    int warm_ok = 0;
    /* phase 0 is the warm-up: an emulator that compiles shaders asynchronously skips draws until the program is
     * ready, so phase-1 content is drawn unchecked until the first frame reads back right (at most WARMUP_MAX).
     * Phases 1..3 are then checked on every frame. */
    for (unsigned phase = 0; phase <= PHASES && !quit; ++phase) {
        if (phase == 2) {
            /* every earlier frame completed (wait_label below), so the RSX no longer reads the old texels */
            arena_fill(tex, TEX_W * TEX_H, COLOR_B);
            cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);
        } else if (phase == 3) {
            arena_quad(vbo, -0.5f, -0.5f, 0.0f, 0.5f); /* right edge at the screen centre */
            cellGcmSetInvalidateVertexCache(ctx);
        }
        unsigned budget = phase == 0 ? WARMUP_MAX : FRAMES_PER_PHASE;
        for (unsigned f = 0; f < budget && !quit; ++f) {
            cellSysutilCheckCallback();
            arena_set_target(ctx, d.color_offset[buffer], d.color_pitch, d.depth_offset, d.depth_pitch, d.width,
                             d.height);
            cellGcmSetClearColor(ctx, COLOR_CLEAR);
            cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
            arena_bind(ctx, &prog, arena_off + ARENA_VBO, arena_off + ARENA_TEX, TEX_W, TEX_H);
            cellGcmSetDrawIndexArray(ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 6, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16,
                                     CELL_GCM_LOCATION_MAIN, arena_off + ARENA_IBO);
            uint32_t wx = d.width / 2 - WIN / 2, wy = d.height / 2 - WIN / 2;
            cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN, arena_off + ARENA_READBACK, WIN * 4, 0, 0,
                                    d.color_offset[buffer], d.color_pitch, wx, wy, WIN, WIN, 4);
            cellGcmSetWriteBackEndLabel(ctx, ARENA_LABEL, ++serial);
            if (present(ctx, (uint8_t)buffer) || wait_label(serial)) {
                printf("GCM_ARENA_FAIL wait phase=%u frame=%u label=0x%08x serial=%u\n", phase, f,
                       *(volatile uint32_t *)cellGcmGetLabelAddress(ARENA_LABEL), serial);
                return 1;
            }
            buffer ^= 1u;
            uint32_t bad = 0;
            uint32_t left = phase <= 1 ? COLOR_A : COLOR_B, right = phase == 3 ? COLOR_CLEAR : left;
            int wrong = check_window(readback, left, right, &bad);
            if (phase == 0) {
                warmup++;
                if (!wrong) {
                    warm_ok = 1;
                    break;
                }
                continue;
            }
            if (wrong) {
                printf("GCM_ARENA_FAIL readback phase=%u frame=%u x=%u got=0x%06x want_left=0x%06x want_right=0x%06x\n",
                       phase, f, bad >> 24, bad & 0xffffffu, left & 0xffffffu, right & 0xffffffu);
                return 1;
            }
            frames++;
            if (f == 0)
                printf("GCM_ARENA_PHASE phase=%u left=0x%06x right=0x%06x ok\n", phase,
                       readback[(WIN / 2) * WIN + 8] & 0xffffffu, readback[(WIN / 2) * WIN + WIN - 8] & 0xffffffu);
        }
        if (phase == 0) {
            if (!warm_ok && !quit) {
                printf("GCM_ARENA_FAIL warm-up: no correct frame in %u\n", WARMUP_MAX);
                return 1;
            }
            printf("GCM_ARENA_WARMUP frames=%u (the last one correct)\n", warmup);
        }
    }
    cellSysutilUnregisterCallback(0);
    if (quit) {
        printf("GCM_ARENA_QUIT frames=%u\n", frames);
        return 0;
    }
    printf("GCM_ARENA_OK frames=%u label=%u serial=%u\n", frames, ARENA_LABEL, serial);
    return 0;
}
