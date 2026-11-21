/*
 * hello-ppu-prof-pixels — known answers for the RSX frame capture's passed-sample counts and zcull state
 * (PS3_PROF_RSX_FRAME, decoder 5).
 *
 * Every frame draws the same list (DRAWS below) on the display surface (W x H, one sample per pixel): full-screen and
 * half-screen quads with depth off, front-to-back and back-to-front pairs with depth LESS, a quad whose fragment
 * program kills half its samples, and a quad inside the app's own ZPASS occlusion query. A zcull region is bound over
 * the depth buffer at start-up and zcull is invalidated before the first depth-tested draw. The app requests three
 * captures itself: frame 30 with PIXELS (counting forced around every draw it has off), frame 60 without (only its
 * own query's draw is counted) and frame 90 BOTH | PIXELS (the same counts pipelined and isolated). tools/check_pixels.py
 * checks the captures against the table; the TTY line PIXELS appquery checks that the capture never changed the
 * app's own query result. At exit, after a proven drain, zcull is turned off behind a fence and the region unbound
 * inside a fourth capture (zcull-unbind) that records the unbind: a binding left behind could outlive the process.
 *
 * GPU safety (as samples/profiling/rsx-frame): every wait is bounded and services callbacks, the fence label is reset
 * and read back first, a failure stops submitting and goes to the exit drain, and if the drain cannot prove the RSX
 * idle the sample parks (no command, no seal, no return).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

#include "vp_quad_vpo.h"
#include "fp_color_fpo.h"
#include "fp_kil_fpo.h"

#if !defined(PS3TC_PROF_AUTO)
#error "hello-ppu-prof-pixels needs PS3_PROF_AUTO=ON (see CMakeLists.txt)"
#endif

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE        0x40000
#define HOST_HEAP_SIZE (16 * 1024 * 1024)
#define FRAMES         120u
#define PIXELS_AT      30u    /* PIXELS: counting forced on around every captured draw the app has it off for */
#define OBSERVE_AT     60u    /* no PIXELS: only the app's own query draw is counted */
#define BOTH_AT        90u    /* BOTH | PIXELS: the same counts pipelined and isolated */
#define LABEL_FENCE    200u
#define APP_REPORT     600u   /* the app's ZPASS report (application range 512..1023) */
#define WAIT_US        2000000ull
#define SESSION_US     60000000ull
#define DRAIN_US       10000000ull

/* What a draw of the list does: a quad rectangle in clip space, its depth, depth test, fragment program, a clear. */
typedef struct draw_desc {
    const char *label;
    int clear;          /* 1 colour + depth + stencil, 2 depth + stencil only, 0 a quad */
    float x0, x1, z;    /* the quad spans x0..x1 (clip space), y -1..1, at depth z */
    int depth_test, kil, app_query, invalidate_zcull;
} draw_desc;
/* The expected passed samples (tools/check_pixels.py keeps the same table): full = W*H, half = W*H/2, none = 0. */
static const draw_desc DRAWS[] = {
    {"clear", 1, 0, 0, 0, 0, 0, 0, 0},
    {"full", 0, -1.0f, 1.0f, 0.0f, 0, 0, 0, 0},          /* W*H */
    {"half", 0, -1.0f, 0.0f, 0.0f, 0, 0, 0, 0},          /* W*H/2 */
    {"clear_depth", 2, 0, 0, 0, 0, 0, 0, 0},
    {"front", 0, -1.0f, 1.0f, -0.5f, 1, 0, 0, 1},        /* W*H, zcull invalidated before it */
    {"behind", 0, -1.0f, 1.0f, 0.5f, 1, 0, 0, 0},        /* 0: every sample behind the front quad */
    {"clear_depth2", 2, 0, 0, 0, 0, 0, 0, 0},
    {"back_first", 0, -1.0f, 1.0f, 0.5f, 1, 0, 0, 0},    /* W*H */
    {"front_second", 0, -1.0f, 1.0f, -0.5f, 1, 0, 0, 0}, /* W*H: in front of the back quad */
    {"kil_half", 0, -1.0f, 1.0f, 0.0f, 0, 1, 0, 0},      /* W*H/2: the program kills uv.x < 0.5 */
    {"app_query", 0, -1.0f, 1.0f, 0.0f, 0, 0, 1, 0},     /* W*H, inside the app's own occlusion query */
};
#define NDRAWS ((uint32_t)(sizeof DRAWS / sizeof DRAWS[0]))

static uint32_t display_width, display_height, color_pitch, depth_pitch, depth_rows;
static uint32_t color_offset[2], depth_offset, cur_buffer;
static uint32_t g_local_heap;
static CGprogram g_vp, g_fp[2];
static void *g_vp_ucode;
static CGparameter g_rect, g_z;
static uint32_t g_fp_offset[2];
static int g_pos;
static uint32_t g_vb_offset;
static CellGcmContextData *g_ctx;
static volatile int g_quit;
static const char *g_failed;
static uint64_t g_start;
static uint32_t g_label_v;
static int g_flip_pending;
static int g_zcull_bound; /* region 0 bound (init_zcull); released on every exit after a proven drain */

static uint64_t now_us(void) { return (uint64_t)sys_time_get_system_time(); }

static void *local_align(uint32_t align, uint32_t size)
{
    uint32_t sz = (size + 0x3ffu) & ~0x3ffu;
    g_local_heap = (g_local_heap + align - 1u) & ~(align - 1u);
    void *p = (void *)(uintptr_t)g_local_heap;
    g_local_heap += sz;
    return p;
}

static void on_sysutil(uint64_t status, uint64_t param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        g_quit = 1;
}

static void fail(const char *where)
{
    if (!g_failed) {
        g_failed = where;
        printf("PIXELS FAIL at=%s elapsed_us=%llu\n", where, (unsigned long long)(now_us() - g_start));
        fflush(stdout);
    }
}

static int flip_done(uint32_t v) { (void)v; return cellGcmGetFlipStatus() == 0; }
static int fence_done(uint32_t v) { return cellGcmIsLabelPassed(LABEL_FENCE, v) != 0; }

static int await(int (*ready)(uint32_t), uint32_t v, uint64_t limit_us, int session_bound)
{
    uint64_t t0 = now_us();
    for (;;) {
        if (ready(v))
            return 1;
        uint64_t t = now_us();
        if (t - t0 >= limit_us || (session_bound && t - g_start >= SESSION_US))
            return 0;
        (void)cellSysutilCheckCallback();
        usleep(50);
    }
}

static void recovery_required(const char *where) __attribute__((noreturn));
static void recovery_required(const char *where)
{
    printf("PIXELS RECOVERY_REQUIRED at=%s no_seal=1 no_return=1\n", where);
    fflush(stdout);
    for (;;)
        usleep(1000000);
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
    if (display_width < 320 || display_height < 240 || (display_width & 1u))
        return 0;
    color_pitch = (display_width * 4u + 63u) & ~63u;
    /* the zcull region covers the depth buffer in 64 x 64 tiles: the buffer is allocated to the aligned size */
    depth_pitch = (((display_width + 63u) & ~63u) * 4u + 63u) & ~63u;
    depth_rows = (display_height + 63u) & ~63u;
    memset(&vcfg, 0, sizeof vcfg);
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch = color_pitch;
    vcfg.aspect = state.displayMode.aspect;
    if (videoConfigure(0, &vcfg, NULL, 0) != 0)
        return 0;
    for (int i = 0; i < 2; i++) {
        void *p = local_align(64, color_pitch * display_height);
        if (cellGcmAddressToOffset(p, &color_offset[i]) != 0)
            return 0;
        if (cellGcmSetDisplayBuffer((uint8_t)i, color_offset[i], color_pitch, display_width, display_height) != 0)
            return 0;
    }
    return cellGcmAddressToOffset(local_align(CELL_GCM_ZCULL_ALIGN_OFFSET, depth_pitch * depth_rows),
                                  &depth_offset) == 0;
}

static int init_programs(void)
{
    const void *fps[2] = {fp_color_fpo, fp_kil_fpo};
    uint32_t size = 0;
    g_vp = (CGprogram)vp_quad_vpo;
    cellGcmCgInitProgram(g_vp);
    cellGcmCgGetUCode(g_vp, &g_vp_ucode, &size);
    g_rect = cellGcmCgGetNamedParameter(g_vp, "rect");
    g_z = cellGcmCgGetNamedParameter(g_vp, "z");
    {
        CGparameter a = cellGcmCgGetNamedParameter(g_vp, "in_position");
        g_pos = a ? (int)cellGcmCgGetParameterResource(g_vp, a) - CG_ATTR0 : 0;
    }
    if (!g_vp_ucode || !size || !g_rect || !g_z)
        return 0;
    for (int i = 0; i < 2; i++) {
        void *blob = NULL;
        g_fp[i] = (CGprogram)fps[i];
        cellGcmCgInitProgram(g_fp[i]);
        cellGcmCgGetUCode(g_fp[i], &blob, &size);
        if (!blob || !size)
            return 0;
        void *dst = local_align(64, size);
        memcpy(dst, blob, size);
        if (cellGcmAddressToOffset(dst, &g_fp_offset[i]) != 0)
            return 0;
    }
    return 1;
}

static int init_geometry(void)
{
    static const float q[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    float *vb = local_align(64, sizeof q);
    memcpy(vb, q, sizeof q);
    return cellGcmAddressToOffset(vb, &g_vb_offset) == 0;
}

/* One zcull region over the whole depth buffer (64-aligned), LESS: binding it is recorded by the capture. */
static int init_zcull(void)
{
    if (cellGcmBindZcull(0, depth_offset, (display_width + 63u) & ~63u, depth_rows, 0, CELL_GCM_ZCULL_Z24S8,
                         CELL_GCM_SURFACE_CENTER_1, CELL_GCM_ZCULL_LESS, CELL_GCM_ZCULL_LONES,
                         CELL_GCM_SCULL_SFUNC_ALWAYS, 0, 0xff) != 0)
        return 0;
    g_zcull_bound = 1;
    return 1;
}

/* The region's unbind (cellGcmUnbindZcull, hooked: the capture records it). Only with the RSX idle or before any GPU
 * work; a no-op returning 0 when nothing is bound. */
static int release_zcull(void)
{
    if (!g_zcull_bound)
        return 0;
    g_zcull_bound = 0;
    return (int)cellGcmUnbindZcull(0);
}

static void surface(void)
{
    CellGcmSurface sf;
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0] = color_offset[cur_buffer];
    sf.colorPitch[0] = color_pitch;
    for (int i = 1; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i] = 64;
    }
    sf.depthFormat = CELL_GCM_SURFACE_Z24S8;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = depth_offset;
    sf.depthPitch = depth_pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = display_width;
    sf.height = display_height;
    cellGcmSetSurface(g_ctx, &sf);
    float scale[4] = {(float)display_width * 0.5f, (float)display_height * -0.5f, 0.5f, 0.0f};
    float offset[4] = {(float)display_width * 0.5f, (float)display_height * 0.5f, 0.5f, 0.0f};
    cellGcmSetViewport(g_ctx, 0, 0, display_width, display_height, 0.0f, 1.0f, scale, offset);
    cellGcmSetScissor(g_ctx, 0, 0, display_width, display_height);
    cellGcmSetColorMask(g_ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(g_ctx, 0);
    cellGcmSetStencilTestEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetAlphaTestEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetBlendEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetCullFaceEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetLogicOpEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetZcullEnable(g_ctx, CELL_GCM_TRUE, CELL_GCM_FALSE);
    cellGcmSetVertexProgram(g_ctx, g_vp, g_vp_ucode);
    cellGcmSetVertexDataArray(g_ctx, (uint8_t)g_pos, 0, 8, 2, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, g_vb_offset);
    cellGcmSetVertexAttribInputMask(g_ctx, (uint16_t)(1u << g_pos));
    cellGcmSetVertexAttribOutputMask(g_ctx, GCM_ATTRIB_OUTPUT_MASK_TEX0);
}

static void render_frame(void)
{
    surface();
    PS3DK_PROF_DRAW_SCOPE_BEGIN("pixels");
    for (uint32_t k = 0; k < NDRAWS; k++) {
        const draw_desc *d = &DRAWS[k];
        PS3DK_PROF_DRAW_LABEL(d->label);
        if (d->clear) {
            cellGcmSetClearColor(g_ctx, 0xff102030u);
            cellGcmSetClearDepthStencil(g_ctx, 0xffffff00u);
            cellGcmSetClearSurface(g_ctx, d->clear == 1 ? (CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B |
                                   CELL_GCM_CLEAR_A | CELL_GCM_CLEAR_Z | CELL_GCM_CLEAR_S)
                                   : (CELL_GCM_CLEAR_Z | CELL_GCM_CLEAR_S));
            continue;
        }
        {
            float rect[4] = {d->x0, -1.0f, d->x1, 1.0f}, z[4] = {d->z, 0.0f, 0.0f, 0.0f};
            cellGcmSetVertexProgramParameter(g_ctx, g_rect, rect);
            cellGcmSetVertexProgramParameter(g_ctx, g_z, z);
        }
        cellGcmSetFragmentProgram(g_ctx, g_fp[d->kil], g_fp_offset[d->kil]);
        cellGcmSetDepthTestEnable(g_ctx, d->depth_test ? CELL_GCM_TRUE : CELL_GCM_FALSE);
        cellGcmSetDepthFunc(g_ctx, CELL_GCM_LESS);
        cellGcmSetDepthMask(g_ctx, d->depth_test ? CELL_GCM_TRUE : CELL_GCM_FALSE);
        if (d->invalidate_zcull)
            cellGcmSetInvalidateZcull(g_ctx);
        if (d->app_query) {
            /* the app's own occlusion query: reset, count, report, stop */
            cellGcmSetClearReport(g_ctx, CELL_GCM_ZPASS_PIXEL_CNT);
            cellGcmSetZpassPixelCountEnable(g_ctx, CELL_GCM_TRUE);
        }
        cellGcmSetInvalidateVertexCache(g_ctx);
        cellGcmSetDrawArrays(g_ctx, CELL_GCM_PRIMITIVE_QUADS, 0, 4);
        if (d->app_query) {
            cellGcmSetReport(g_ctx, CELL_GCM_ZPASS_PIXEL_CNT, APP_REPORT);
            cellGcmSetZpassPixelCountEnable(g_ctx, CELL_GCM_FALSE);
        }
    }
    PS3DK_PROF_DRAW_SCOPE_END();
}

static int submit_flip(void)
{
    cellGcmResetFlipStatus();
    if (cellGcmSetFlip(g_ctx, (uint8_t)cur_buffer) != 0) {
        fail("submit-flip");
        return 0;
    }
    cellGcmFlush(g_ctx);
    g_flip_pending = 1;
    cur_buffer ^= 1u;
    return 1;
}

int main(void)
{
    sys_addr_t host = 0;
    CellGcmConfig cfg;
    int ids[3] = {0, 0, 0};
    uint32_t frames = 0, query_bad = 0;

    (void)PS3DK_PROF_CAPTURE_WHOLE_RUN();
    if (gcm_host_buffer_alloc(HOST_HEAP_SIZE, &host) != 0 ||
        cellGcmInit(CB_SIZE, HOST_HEAP_SIZE, (void *)(uintptr_t)host) != 0) {
        printf("PIXELS init failed (host buffer or cellGcmInit)\n");
        return 1;
    }
    cellGcmGetConfiguration(&cfg);
    g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;
    g_ctx = CELL_GCM_CURRENT;
    if (!init_display() || cellSysutilRegisterCallback(0, on_sysutil, NULL) != 0) {
        printf("PIXELS init failed (display or callback)\n");
        return 1;
    }
    volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_FENCE);
    *label = 0;
    __asm__ volatile("sync" ::: "memory");
    if (*label != 0) {
        printf("PIXELS label %u did not reset (0x%08x)\n", LABEL_FENCE, (unsigned)*label);
        return 1;
    }
    if (!init_programs() || !init_geometry()) {
        printf("PIXELS init failed (programs or geometry)\n");
        return 1;
    }
    /* the zcull bind is the last init step and no GPU work has run before it: a failure after it would unbind at
     * once (release_zcull); none follows it today */
    if (!init_zcull()) {
        printf("PIXELS init failed (zcull bind)\n");
        return 1;
    }
    printf("PIXELS start frames=%u width=%u height=%u draws_per_frame=%u\n", FRAMES, display_width, display_height,
           NDRAWS);
    fflush(stdout);

    g_start = now_us();
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    for (frames = 0; frames < FRAMES && !g_failed && !g_quit; frames++) {
        if (cellSysutilCheckCallback() != 0) {
            fail("sysutil-check");
            break;
        }
        if (now_us() - g_start >= SESSION_US) {
            fail("session-deadline");
            break;
        }
        if (frames == PIXELS_AT)
            ids[0] = ps3dk_prof_rsx_frame_capture("pixels", PS3DK_PROF_RSX_FRAME_PIXELS);
        if (frames == OBSERVE_AT)
            ids[1] = ps3dk_prof_rsx_frame_capture("observe", 0u);
        if (frames == BOTH_AT)
            ids[2] = ps3dk_prof_rsx_frame_capture("pixels-both", PS3DK_PROF_RSX_FRAME_BOTH | PS3DK_PROF_RSX_FRAME_PIXELS);
        if (frames == PIXELS_AT || frames == OBSERVE_AT || frames == BOTH_AT) {
            printf("PIXELS request frame=%u result=%d\n", frames,
                   frames == PIXELS_AT ? ids[0] : frames == OBSERVE_AT ? ids[1] : ids[2]);
            fflush(stdout);
        }
        render_frame();
        if (!submit_flip())
            break;
        if (!await(flip_done, 0, WAIT_US, 1)) {
            fail("flip-wait");
            break;
        }
        g_flip_pending = 0;
        {
            /* the flip completed after the frame's report: the app's query result must be the full screen whatever
             * the capture did (frames 31, 61, 91, 92 are the captured ones) */
            const uint32_t value = cellGcmGetReportDataAddress(APP_REPORT)->value;
            const uint32_t want = display_width * display_height;
            const int captured = frames == PIXELS_AT + 1 || frames == OBSERVE_AT + 1 || frames == BOTH_AT + 1 ||
                                 frames == BOTH_AT + 2;
            /* the verdict is the captured frames' (an emulator may deliver the first frames' query results late) */
            if (captured && value != want)
                query_bad++;
            if (captured || value != want) {
                printf("PIXELS appquery frame=%u value=%u expect=%u\n", frames, value, want);
                fflush(stdout);
            }
        }
    }

    int flip_ok = !g_flip_pending || await(flip_done, 0, DRAIN_US, 0);
    int fence_ok = 0;
    if (flip_ok) {
        g_label_v++;
        cellGcmSetWriteBackEndLabel(g_ctx, LABEL_FENCE, g_label_v);
        cellGcmFlush(g_ctx);
        fence_ok = await(fence_done, g_label_v, DRAIN_US, 0);
    }
    printf("PIXELS drain flip=%s fence=%s\n", flip_ok ? "ok" : "timeout", fence_ok ? "ok" : "timeout");
    fflush(stdout);
    if (!flip_ok || !fence_ok)
        recovery_required(flip_ok ? "final-fence" : "final-flip");
    /* The RSX is idle. Release the zcull region before the seal: a binding is driver state that may outlive the
     * process, and the next app placing its depth buffer in this local memory would cull against it. Zcull off, a
     * fenced flush, then the unbind, inside one more capture (zcull-unbind) so the capture records the unbind. */
    int zid = ps3dk_prof_rsx_frame_capture("zcull-unbind", 0u);
    PS3DK_PROF_FRAME(); /* arms the capture */
    cellGcmSetZcullEnable(g_ctx, CELL_GCM_FALSE, CELL_GCM_FALSE);
    g_label_v++;
    cellGcmSetWriteBackEndLabel(g_ctx, LABEL_FENCE, g_label_v);
    cellGcmFlush(g_ctx);
    if (!await(fence_done, g_label_v, DRAIN_US, 0))
        recovery_required("zcull-off-fence");
    int unbind_rc = release_zcull();
    printf("PIXELS zcull unbind rc=%d capture=%d\n", unbind_rc, zid);
    fflush(stdout);
    PS3DK_PROF_FRAME(); /* closes it */
    cellSysutilUnregisterCallback(0);
    PS3DK_PROF_AUTO_SEAL();
    if (g_failed) {
        printf("PIXELS EXIT failed at=%s frames=%u\n", g_failed, frames);
        return 1;
    }
    printf("PIXELS EXIT %s frames=%u captures=%d,%d,%d,%d appquery_mismatches=%u zcull_unbind=%d\n",
           g_quit ? "quit" : "clean", frames, ids[0], ids[1], ids[2], zid, query_bad, unbind_rc);
    return ids[0] > 0 && ids[1] > 0 && ids[2] > 0 && zid > 0 && !query_bad && unbind_rc == 0 ? 0 : 2;
}
