/*
 * hello-ppu-prof-auto-verdicts — known-answer phases for AUTO frame verdicts.
 *
 * Exercises the automatic RSX profiler's per-frame verdicts on PS3 hardware
 * across five distinct bottleneck phases:
 *   (A) RSX-bound (heavy fragment loops with SetWaitFlip and flip-wait complete)
 *   (B) PPU-bound (trivial RSX work, ~45 ms PPU busy loop per frame under HSYNC)
 *   (C) Pacing (trivial RSX work, VSYNC flip waiting on display hardware)
 *   (D) RSX-bound waiting by flip polling (polling cellGcmGetFlipStatus)
 *   (E) RSX-bound waiting by label polling (polling cellGcmIsLabelPassed on label 200)
 *
 * Each phase runs for 60 frames (300 frames total). Frames are delimited by
 * flip submission. Before process return, PS3DK_PROF_AUTO_SEAL() is called.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>

#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <sysutil/video.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <Cg/cg.h>
#ifndef PS3DK_PROF
#define PS3DK_PROF 1
#endif
#include <ps3dk/prof.h>
#include <ps3dk/prof_auto.h>

#include "../../common/gcm_host_buffer.h"

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE             0x40000
#define HOST_HEAP_SIZE      (32 * 1024 * 1024)
#define COLOR_BUFFERS       2
#define FRAMES_PER_PHASE    60
#define HEAVY_PASSES        4
#define LABEL_PHASE_E       200

typedef struct {
    float pos[2];
    float color[4];
    float uv[2];
} vertex_t;

static uint32_t display_width;
static uint32_t display_height;
static uint32_t color_pitch;
static uint32_t depth_pitch;
static uint32_t color_offset[COLOR_BUFFERS];
static uint32_t depth_offset;
static uint32_t cur_buffer = 0;

static uint32_t g_local_heap;
static void *local_align(uint32_t align, uint32_t size)
{
    uint32_t sz = (size + 0x3ff) & ~0x3ffu;
    g_local_heap = (g_local_heap + align - 1) & ~(align - 1u);
    void *p = (void *)(uintptr_t)g_local_heap;
    g_local_heap += sz;
    return p;
}

static CGprogram g_vp;
static CGprogram g_fp;
static void *g_vp_ucode;
static uint32_t g_fp_offset;
static int g_pos_attr;
static int g_col_attr;
static int g_uv_attr;

static vertex_t *g_vb;
static uint32_t g_vb_offset;

static volatile int g_quit_requested = 0;

static void on_sysutil(uint64_t status, uint64_t param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        g_quit_requested = 1;
}

static int init_rsx(void *host_addr)
{
    CellGcmConfig cfg;
    if (cellGcmInit(CB_SIZE, HOST_HEAP_SIZE, host_addr) != 0)
        return 0;
    cellGcmGetConfiguration(&cfg);
    g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;
    return 1;
}

static int init_display(void)
{
    videoState state;
    videoResolution res;
    videoConfiguration vcfg;

    if (videoGetState(0, 0, &state) != 0 || state.state != 0)
        return 0;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0)
        return 0;

    display_width = res.width;
    display_height = res.height;
    if (display_width < 320 || display_height < 240)
        return 0;

    color_pitch = (display_width * 4 + 63) & ~63u;
    depth_pitch = (display_width * 2 + 63) & ~63u;

    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch = color_pitch;
    vcfg.aspect = state.displayMode.aspect;

    if (videoConfigure(0, &vcfg, NULL, 0) != 0)
        return 0;

    uint32_t color_size = color_pitch * display_height;
    uint32_t depth_size = depth_pitch * display_height;

    for (int i = 0; i < COLOR_BUFFERS; i++) {
        void *p = local_align(64, color_size);
        if (cellGcmAddressToOffset(p, &color_offset[i]) != 0)
            return 0;
        if (cellGcmSetDisplayBuffer((uint8_t)i, color_offset[i], color_pitch, display_width, display_height) != 0)
            return 0;
    }

    void *dp = local_align(64, depth_size);
    if (cellGcmAddressToOffset(dp, &depth_offset) != 0)
        return 0;

    return 1;
}

static void init_shaders(void)
{
    uint32_t vpsize = 0, fpsize = 0;
    g_vp = (CGprogram)vpshader_vpo;
    g_fp = (CGprogram)fpshader_fpo;

    cellGcmCgInitProgram(g_vp);
    cellGcmCgInitProgram(g_fp);

    cellGcmCgGetUCode(g_vp, &g_vp_ucode, &vpsize);

    void *fp_blob;
    cellGcmCgGetUCode(g_fp, &fp_blob, &fpsize);
    uint32_t fp_copy = (uint32_t)fpsize;
    uint32_t *dst = (uint32_t *)local_align(64, fp_copy ? fp_copy : 4);
    memcpy(dst, fp_blob, fp_copy);
    cellGcmAddressToOffset(dst, &g_fp_offset);

    CGparameter pos = cellGcmCgGetNamedParameter(g_vp, "in_position");
    CGparameter col = cellGcmCgGetNamedParameter(g_vp, "in_color");
    CGparameter uv  = cellGcmCgGetNamedParameter(g_vp, "in_uv");
    g_pos_attr = pos ? (int)cellGcmCgGetParameterResource(g_vp, pos) - CG_ATTR0 : 0;
    g_col_attr = col ? (int)cellGcmCgGetParameterResource(g_vp, col) - CG_ATTR0 : 1;
    g_uv_attr  = uv  ? (int)cellGcmCgGetParameterResource(g_vp, uv)  - CG_ATTR0 : 2;
}

static void init_geometry(void)
{
    g_vb = (vertex_t *)local_align(64, 4 * sizeof(vertex_t));
    /* Full-screen quad in NDC coordinates [-1, 1] */
    g_vb[0].pos[0] = -1.0f; g_vb[0].pos[1] = -1.0f;
    g_vb[1].pos[0] =  1.0f; g_vb[1].pos[1] = -1.0f;
    g_vb[2].pos[0] =  1.0f; g_vb[2].pos[1] =  1.0f;
    g_vb[3].pos[0] = -1.0f; g_vb[3].pos[1] =  1.0f;
    for (int i = 0; i < 4; i++) {
        g_vb[i].color[0] = 1.0f; g_vb[i].color[1] = 0.8f;
        g_vb[i].color[2] = 0.6f; g_vb[i].color[3] = 1.0f;
        g_vb[i].uv[0] = (i == 1 || i == 2) ? 1.0f : 0.0f;
        g_vb[i].uv[1] = (i >= 2) ? 1.0f : 0.0f;
    }
    cellGcmAddressToOffset(g_vb, &g_vb_offset);
}

static void setup_render_state(CellGcmContextData *ctx)
{
    CellGcmSurface sf;
    memset(&sf, 0, sizeof(sf));
    sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget      = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = color_offset[cur_buffer];
    sf.colorPitch[0]    = color_pitch;
    for (int i = 1; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i]    = 64;
    }
    sf.depthFormat      = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation    = GCM_LOCATION_RSX;
    sf.depthOffset      = depth_offset;
    sf.depthPitch       = depth_pitch;
    sf.type             = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias        = GCM_SURFACE_CENTER_1;
    sf.width            = display_width;
    sf.height           = display_height;
    sf.x                = 0;
    sf.y                = 0;
    cellGcmSetSurface(ctx, &sf);

    float minz = 0.0f, maxz = 1.0f;
    float scale[4]  = { (float)display_width * 0.5f, (float)display_height * -0.5f, 0.5f, 0.0f };
    float offset[4] = { (float)display_width * 0.5f, (float)display_height *  0.5f, 0.5f, 0.0f };
    cellGcmSetViewport(ctx, 0, 0, display_width, display_height, minz, maxz, scale, offset);

    cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(ctx, 0);

    cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
    cellGcmSetDepthMask(ctx, GCM_FALSE);
    cellGcmSetStencilTestEnable(ctx, GCM_FALSE);
    cellGcmSetBlendEnable(ctx, GCM_FALSE);
    cellGcmSetLogicOpEnable(ctx, GCM_FALSE);
    cellGcmSetCullFaceEnable(ctx, GCM_FALSE);

    cellGcmSetVertexProgram(ctx, g_vp, g_vp_ucode);
    cellGcmSetFragmentProgram(ctx, g_fp, g_fp_offset);

    cellGcmSetVertexDataArray(ctx, g_pos_attr, 0, (uint8_t)sizeof(vertex_t), 2,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
                              g_vb_offset + (uint32_t)offsetof(vertex_t, pos));
    cellGcmSetVertexDataArray(ctx, g_col_attr, 0, (uint8_t)sizeof(vertex_t), 4,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
                              g_vb_offset + (uint32_t)offsetof(vertex_t, color));
    cellGcmSetVertexDataArray(ctx, g_uv_attr, 0, (uint8_t)sizeof(vertex_t), 2,
                              CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
                              g_vb_offset + (uint32_t)offsetof(vertex_t, uv));

    cellGcmSetVertexAttribInputMask(ctx, (uint16_t)((1u << g_pos_attr) |
                                                    (1u << g_col_attr) |
                                                    (1u << g_uv_attr)));
    cellGcmSetVertexAttribOutputMask(ctx, GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | GCM_ATTRIB_OUTPUT_MASK_TEX0);
}

static void draw_heavy_quads(CellGcmContextData *ctx, int passes)
{
    cellGcmSetScissor(ctx, 0, 0, display_width, display_height);
    cellGcmSetClearColor(ctx, 0xff101824);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    for (int i = 0; i < passes; i++) {
        cellGcmSetInvalidateVertexCache(ctx);
        cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
    }
}

static void draw_trivial(CellGcmContextData *ctx)
{
    cellGcmSetScissor(ctx, 0, 0, display_width, display_height);
    cellGcmSetClearColor(ctx, 0xff182430);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    cellGcmSetScissor(ctx, 0, 0, 16, 16);
    cellGcmSetInvalidateVertexCache(ctx);
    cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
}

static int submit_flip(CellGcmContextData *ctx)
{
    cellGcmResetFlipStatus();
    if (cellGcmSetFlip(ctx, (uint8_t)cur_buffer) != 0)
        return 0;
    cellGcmFlush(ctx);
    cur_buffer ^= 1u;
    return 1;
}

static void print_phase_banner(char phase)
{
    ps3dk_prof_token tok = PS3DK_PROF_FRAME_TOKEN();
    if (tok.owner == 0) {
        printf("hello-ppu-prof-auto-verdicts: phase %c profiler token has no owner\n", phase);
        abort();
    }
    printf("AUTOVERDICT phase %c frame=%llu\n", phase, (unsigned long long)tok.frame);
    fflush(stdout);
}

int main(void)
{
    sys_addr_t host_addr = 0;

    if (gcm_host_buffer_alloc(HOST_HEAP_SIZE, &host_addr) != 0) {
        printf("hello-ppu-prof-auto-verdicts: gcm_host_buffer_alloc failed\n");
        return 1;
    }
    if (!init_rsx((void *)(uintptr_t)host_addr)) {
        printf("hello-ppu-prof-auto-verdicts: init_rsx failed\n");
        return 1;
    }
    if (!init_display()) {
        printf("hello-ppu-prof-auto-verdicts: init_display failed\n");
        return 1;
    }

    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    if (cellSysutilRegisterCallback(0, on_sysutil, NULL) != 0) {
        printf("hello-ppu-prof-auto-verdicts: callback registration failed\n");
        return 1;
    }

    /* Reset label memory to 0 with sync and confirm before any GPU work */
    volatile uint32_t *label_addr = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_PHASE_E);
    *label_addr = 0;
    __asm__ volatile("sync" ::: "memory");
    if (*label_addr != 0) {
        printf("hello-ppu-prof-auto-verdicts: label %u failed to zero (read 0x%08x)\n", LABEL_PHASE_E, *label_addr);
        return 1;
    }

    init_shaders();
    init_geometry();

    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    setup_render_state(ctx);

    /* Initial flip submission to open Frame 0 */
    if (!submit_flip(ctx)) {
        printf("hello-ppu-prof-auto-verdicts: initial flip submission failed\n");
        return 1;
    }

    /* ---- Phase A: RSX-bound (frames 0..59) ---- */
    print_phase_banner('A');

    for (int i = 0; i < FRAMES_PER_PHASE && !g_quit_requested; i++) {
        if (cellSysutilCheckCallback() != 0) break;
        setup_render_state(ctx);
        cellGcmSetWaitFlip(ctx);
        draw_heavy_quads(ctx, HEAVY_PASSES);
        submit_flip(ctx);
        cellGcmWaitFlipComplete(2000000);
    }

    /* ---- Phase B: PPU-bound (frames 60..119) ---- */
    /* Switch to HSYNC flip mode and use 45 ms spin to avoid landing near VSYNC
     * interval multiples (~33.3 ms / 2 vsyncs), avoiding PPU work vs presentation
     * pacing ambiguity in profiler analysis. */
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_HSYNC);
    print_phase_banner('B');

    for (int i = 0; i < FRAMES_PER_PHASE && !g_quit_requested; i++) {
        if (cellSysutilCheckCallback() != 0) break;
        setup_render_state(ctx);
        draw_trivial(ctx);
        /* PPU-bound: spin on a busy loop for ~45 ms (non-vsync multiple under HSYNC) */
        uint64_t start_us = sys_time_get_system_time();
        while (sys_time_get_system_time() - start_us < 45000) {
            __asm__ volatile("" ::: "memory");
        }
        submit_flip(ctx);
    }

    /* ---- Phase C: Pacing (frames 120..179) ---- */
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    print_phase_banner('C');

    for (int i = 0; i < FRAMES_PER_PHASE && !g_quit_requested; i++) {
        if (cellSysutilCheckCallback() != 0) break;
        setup_render_state(ctx);
        draw_trivial(ctx);
        submit_flip(ctx);
        cellGcmWaitFlipComplete(2000000);
    }

    /* ---- Phase D: RSX-bound waiting by flip polling (frames 180..239) ---- */
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_HSYNC);
    print_phase_banner('D');

    for (int i = 0; i < FRAMES_PER_PHASE && !g_quit_requested; i++) {
        if (cellSysutilCheckCallback() != 0) break;
        setup_render_state(ctx);
        cellGcmSetWaitFlip(ctx);
        draw_heavy_quads(ctx, HEAVY_PASSES);
        submit_flip(ctx);
        while (cellGcmGetFlipStatus() != 0) {
            usleep(20);
        }
    }

    /* ---- Phase E: RSX-bound waiting by label polling (frames 240..299) ---- */
    print_phase_banner('E');

    uint32_t label_v = 0;

    for (int i = 0; i < FRAMES_PER_PHASE && !g_quit_requested; i++) {
        if (cellSysutilCheckCallback() != 0) break;
        setup_render_state(ctx);
        cellGcmSetWaitFlip(ctx);
        draw_heavy_quads(ctx, HEAVY_PASSES);
        label_v++;
        cellGcmSetWriteBackEndLabel(ctx, LABEL_PHASE_E, label_v);
        cellGcmFlush(ctx);
        while (!cellGcmIsLabelPassed(LABEL_PHASE_E, label_v)) {
            usleep(20);
        }
        submit_flip(ctx);
    }

    /* Exit sequence: ensure RSX and display state are clean */
    /* 1. Bounded wait, at most 2 s, polling cellGcmGetFlipStatus() == 0 with usleep(1000) */
    for (int n = 0; cellGcmGetFlipStatus() != 0 && n < 2000; n++) {
        usleep(1000);
    }

    /* 2. FRESH label value written with cellGcmSetWriteBackEndLabel, bounded 2 s fence */
    int fence_ok = 0;
    label_v++;
    cellGcmSetWriteBackEndLabel(ctx, LABEL_PHASE_E, label_v);
    cellGcmFlush(ctx);
    for (int n = 0; n < 2000; n++) {
        if (cellGcmIsLabelPassed(LABEL_PHASE_E, label_v)) {
            fence_ok = 1;
            break;
        }
        usleep(1000);
    }
    printf("AUTOVERDICT EXIT fence=%s\n", fence_ok ? "ok" : "timeout");
    fflush(stdout);

    /* 3. Unregister the sysutil callback */
    cellSysutilUnregisterCallback(0);

    /* 4. Seal automatic profiler capture before process return */
    PS3DK_PROF_AUTO_SEAL();

    /* 5. Print 'AUTOVERDICT EXIT clean' last */
    if (!fence_ok) {
        return 1;
    }

    printf("AUTOVERDICT EXIT clean\n");
    fflush(stdout);
    return 0;
}
