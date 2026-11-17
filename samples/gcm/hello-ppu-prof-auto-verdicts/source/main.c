/*
 * hello-ppu-prof-auto-verdicts — known-answer phases for AUTO frame verdicts.
 *
 * Exercises the automatic RSX profiler's per-frame verdicts on PS3 hardware
 * across five distinct bottleneck phases:
 *   (A) RSX-bound (heavy fragment loops with SetWaitFlip and flip-wait complete)
 *   (B) PPU busy (trivial RSX work, ~45 ms PPU busy loop per frame under HSYNC).
 *       The reader answers UNKNOWN "RSX not limiting; PPU work vs presentation
 *       pacing not distinguished": a PPU-bound verdict needs positive PPU-busy
 *       evidence, which this capture does not carry.
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

#include "verdicts_logic.h"
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

static CellGcmContextData *g_ctx;
static av_state g_run;
static uint64_t g_session_start;
static uint32_t g_label_v;

static uint64_t now_us(void *u) { (void)u; return (uint64_t)sys_time_get_system_time(); }
static int flip_done(void *u, uint32_t arg) { (void)u; (void)arg; return cellGcmGetFlipStatus() == 0; }
static int label_done(void *u, uint32_t v) { (void)u; return cellGcmIsLabelPassed(LABEL_PHASE_E, v) != 0; }
static void poll_idle(void *u) { (void)u; (void)cellSysutilCheckCallback(); usleep(20); }
static int quit_requested(void *u) { (void)u; return g_quit_requested; }

static void fail(const char *where)
{
    if (!g_run.failed) {
        printf("AUTOVERDICT FAIL at=%s elapsed_us=%llu\n", where,
               (unsigned long long)av_since(now_us(0), g_session_start));
        fflush(stdout);
    }
    av_fail(&g_run, where);
}

/* Terminal operator recovery state: the RSX was not proven idle, so issue no GPU command, do not seal, free, return
 * or exitspawn. An injected run-hook watchdog can still override this at its own timeout. */
static void recovery_required(const char *where) __attribute__((noreturn));
static void recovery_required(const char *where)
{
    printf("AUTOVERDICT RECOVERY_REQUIRED at=%s no_seal=1 no_return=1\n", where);
    fflush(stdout);
    for (;;)
        usleep(1000000);
}

/* Poll until `ready(arg)`, a deadline (recorded as a failure at `where`) or a quit request. */
static int await_ready(int (*ready)(void *, uint32_t), uint32_t arg, const char *where)
{
    av_clock c = { now_us, ready, poll_idle, quit_requested, 0 };
    int d = av_await(&c, arg, g_session_start, AV_SESSION_US, 1);
    if (d == AV_QUIT)
        g_run.quit = 1;
    else if (d != AV_DONE)
        fail(where);
    return d == AV_DONE;
}

static int g_flip_unqueued; /* the last submission was refused: its reset status awaits no flip */

static int submit_flip(void)
{
    cellGcmResetFlipStatus();
    if (cellGcmSetFlip(g_ctx, (uint8_t)cur_buffer) != 0) {
        g_flip_unqueued = 1;
        fail("submit-flip");
        return 0;
    }
    cellGcmFlush(g_ctx);
    cur_buffer ^= 1u;
    return 1;
}

static int wait_flip_complete(void)
{
    if (cellGcmWaitFlipComplete((uint32_t)AV_WAIT_US) != 0) {
        fail("wait-flip-complete");
        return 0;
    }
    return 1;
}

/* Per frame: service system callbacks and check the session budget before submitting anything. */
static int frame_begin(void)
{
    if (cellSysutilCheckCallback() != 0) {
        fail("sysutil-check");
        return 0;
    }
    if (g_quit_requested)
        g_run.quit = 1;
    if (av_since(now_us(0), g_session_start) >= AV_SESSION_US) {
        fail("session-deadline");
        return 0;
    }
    return !av_stopping(&g_run);
}

/* Each frame returns 0 after recording a failure (fail); a quit or an earlier stop returns 1 with no work done. */
static int frame_a(void *u, int i)
{
    (void)u; (void)i;
    if (!frame_begin()) return 1;
    setup_render_state(g_ctx);
    cellGcmSetWaitFlip(g_ctx);
    draw_heavy_quads(g_ctx, HEAVY_PASSES);
    return submit_flip() && wait_flip_complete();
}

static int frame_b(void *u, int i)
{
    (void)u; (void)i;
    if (!frame_begin()) return 1;
    setup_render_state(g_ctx);
    draw_trivial(g_ctx);
    /* PPU busy: spin on a busy loop for ~45 ms (non-vsync multiple under HSYNC) */
    uint64_t start_us = now_us(0);
    while (av_since(now_us(0), start_us) < 45000) {
        __asm__ volatile("" ::: "memory");
    }
    return submit_flip();
}

static int frame_c(void *u, int i)
{
    (void)u; (void)i;
    if (!frame_begin()) return 1;
    setup_render_state(g_ctx);
    draw_trivial(g_ctx);
    return submit_flip() && wait_flip_complete();
}

static int frame_d(void *u, int i)
{
    (void)u; (void)i;
    if (!frame_begin()) return 1;
    setup_render_state(g_ctx);
    cellGcmSetWaitFlip(g_ctx);
    draw_heavy_quads(g_ctx, HEAVY_PASSES);
    if (!submit_flip()) return 0;
    return await_ready(flip_done, 0, "flip-status-poll") || g_run.quit;
}

static int frame_e(void *u, int i)
{
    (void)u; (void)i;
    if (!frame_begin()) return 1;
    setup_render_state(g_ctx);
    cellGcmSetWaitFlip(g_ctx);
    draw_heavy_quads(g_ctx, HEAVY_PASSES);
    g_label_v++;
    cellGcmSetWriteBackEndLabel(g_ctx, LABEL_PHASE_E, g_label_v);
    cellGcmFlush(g_ctx);
    if (!await_ready(label_done, g_label_v, "label-poll"))
        return g_run.quit != 0;
    return submit_flip();
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

static void run_phase(char phase, int (*frame)(void *, int))
{
    if (av_stopping(&g_run))
        return;
    print_phase_banner(phase);
    (void)av_phase(&g_run, FRAMES_PER_PHASE, frame, 0);
}

int main(void)
{
    sys_addr_t host_addr = 0;
    /* a profiled build records nothing until a capture window opens: record the whole run */
    (void)PS3DK_PROF_CAPTURE_WHOLE_RUN();

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

    g_ctx = CELL_GCM_CURRENT;
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

    g_session_start = now_us(0);
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    setup_render_state(g_ctx);

    /* Initial flip submission to open Frame 0. From here on every failure, deadline or quit stops the workload at
     * once and goes to the exit drain; nothing is retried. */
    (void)submit_flip();

    /* (A) RSX-bound: heavy fragment work behind cellGcmSetWaitFlip, then cellGcmWaitFlipComplete */
    run_phase('A', frame_a);
    /* (B) PPU busy, RSX not limiting: HSYNC flips and a 45 ms spin, away from VSYNC interval multiples, so the
     * frame is not presentation pacing */
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_HSYNC);
    run_phase('B', frame_b);
    /* (C) Pacing: trivial work, VSYNC flip wait on the display */
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    run_phase('C', frame_c);
    /* (D) RSX-bound waiting by flip-status polling */
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_HSYNC);
    run_phase('D', frame_d);
    /* (E) RSX-bound waiting by label polling */
    run_phase('E', frame_e);

    /* Exit drain, also after a failure or quit: the last flip must complete and a FRESH label fence must pass, each
     * within AV_WAIT_US, before anything is sealed or returned. Otherwise the RSX is not proven idle: terminal
     * recovery. */
    av_clock c = { now_us, flip_done, poll_idle, quit_requested, 0 };
    /* After a refused submission the status was reset for a flip that was never queued, so it cannot be awaited;
     * the fence below still proves the RSX processed every earlier command, including the last queued flip. */
    int flip_ok = g_flip_unqueued || av_await(&c, 0, g_session_start, AV_DRAIN_US, 0) == AV_DONE;
    int fence_ok = 0;
    if (flip_ok) {
        g_label_v++;
        cellGcmSetWriteBackEndLabel(g_ctx, LABEL_PHASE_E, g_label_v);
        cellGcmFlush(g_ctx);
        c.ready = label_done;
        fence_ok = av_await(&c, g_label_v, g_session_start, AV_DRAIN_US, 0) == AV_DONE;
    }
    printf("AUTOVERDICT EXIT flip=%s fence=%s\n", flip_ok ? "ok" : "timeout",
           fence_ok ? "ok" : (flip_ok ? "timeout" : "skipped"));
    fflush(stdout);
    if (av_exit(flip_ok, fence_ok) != AV_EXIT_SEAL)
        recovery_required(flip_ok ? "final-fence" : "final-flip");

    cellSysutilUnregisterCallback(0);
    /* Seal the automatic profiler capture before process return; the RSX is idle. */
    PS3DK_PROF_AUTO_SEAL();

    if (g_run.failed) {
        printf("AUTOVERDICT EXIT failed at=%s\n", g_run.failed);
        fflush(stdout);
        return 1;
    }
    printf(g_run.quit ? "AUTOVERDICT EXIT quit\n" : "AUTOVERDICT EXIT clean\n");
    fflush(stdout);
    return 0;
}
