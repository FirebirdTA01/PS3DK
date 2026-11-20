/*
 * rsx-frame — a known draw matrix for the RSX frame capture (PS3_PROF_RSX_FRAME).
 *
 * Every frame submits the same draws (source/matrix.h): one clear, the 2 VP x 3 FP matrix with three texture formats
 * and depth / cull / blend variants inside the AUTO range "matrix", one indexed draw, and an instance run of 8
 * identical draws that differ only in a vertex-program constant inside the AUTO range "instances", with labels in
 * nested scopes and a tag per group. At frame CAPTURE_AT the app requests a capture itself
 * (PS3DK_PROF_RSX_FRAME_CAPTURE), so no PC app is needed; the PC app can still request more over the live link. At
 * frame BOTH_AT it requests a BOTH capture with FP bytes: the same draws pipelined, then isolated, paired.
 * After FRAMES frames the sample drains the RSX, seals the capture and returns. tools/check_capture.py compares the
 * reader's output for the capture with expected/draws.tsv field by field.
 *
 * GPU safety: every wait (flip, fence) is bounded and polls system callbacks; the fence label is reset and read
 * back before any GPU work; a failure stops submitting at once and goes to the exit drain; if the drain cannot prove
 * the RSX idle the sample parks (no command, no seal, no return).
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

#include "../../../gcm/common/gcm_host_buffer.h"

#include "matrix.h"
#include "vp_offset_vpo.h"
#include "vp_scale_vpo.h"
#include "fp_color_fpo.h"
#include "fp_tex_fpo.h"
#include "fp_texmod_fpo.h"

#if !defined(PS3TC_PROF_AUTO)
#error "rsx-frame needs PS3_PROF_AUTO=ON (see CMakeLists.txt)"
#endif

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE        0x40000
#define HOST_HEAP_SIZE (16 * 1024 * 1024)
#define FRAMES         120u   /* then a clean exit */
#define CAPTURE_AT     30u    /* the frame that requests the capture; the next frame boundary arms it */
#define BOTH_AT        60u    /* the frame that requests a BOTH capture (pipelined then isolated) with FP bytes */
#define LABEL_FENCE    200u
#define WAIT_US        2000000ull   /* any single flip or fence wait */
#define SESSION_US     60000000ull  /* the whole workload */
#define DRAIN_US       10000000ull  /* each exit-drain wait */
#define TEX_SIZE       64u

typedef struct {
    float pos[2];
    float color[4];
    float uv[2];
} vertex_t;

static uint32_t display_width, display_height, color_pitch, depth_pitch;
static uint32_t color_offset[2], depth_offset, cur_buffer;
static uint32_t g_local_heap;

static void *local_align(uint32_t align, uint32_t size)
{
    uint32_t sz = (size + 0x3ffu) & ~0x3ffu;
    g_local_heap = (g_local_heap + align - 1u) & ~(align - 1u);
    void *p = (void *)(uintptr_t)g_local_heap;
    g_local_heap += sz;
    return p;
}

typedef struct vp_info {
    CGprogram prog;
    void *ucode;
    CGparameter constant;  /* "offset" or "xform" */
    int pos, col, uv;      /* attribute indices */
} vp_info;
typedef struct fp_info {
    CGprogram prog;
    uint32_t offset;       /* the ucode copy in local memory */
} fp_info;

static vp_info g_vp[VP_COUNT];
static fp_info g_fp[FP_COUNT];
static uint32_t g_tex_offset[TEX_COUNT];
static vertex_t *g_vb;
static uint32_t g_vb_offset, g_ib_offset;
static CellGcmContextData *g_ctx;
static volatile int g_quit;
static const char *g_failed;
static uint64_t g_start;
static uint32_t g_label_v;
static int g_flip_pending;

static uint64_t now_us(void) { return (uint64_t)sys_time_get_system_time(); }

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
        printf("RSXFRAME FAIL at=%s elapsed_us=%llu\n", where, (unsigned long long)(now_us() - g_start));
        fflush(stdout);
    }
}

static int flip_done(uint32_t v) { (void)v; return cellGcmGetFlipStatus() == 0; }
static int fence_done(uint32_t v) { return cellGcmIsLabelPassed(LABEL_FENCE, v) != 0; }

/* Polls ready(v) until it holds (1), `limit_us` passes or the session budget runs out (0). Services callbacks. */
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

/* The RSX was not proven idle: no further GPU command, no seal, no return. */
static void recovery_required(const char *where) __attribute__((noreturn));
static void recovery_required(const char *where)
{
    printf("RSXFRAME RECOVERY_REQUIRED at=%s no_seal=1 no_return=1\n", where);
    fflush(stdout);
    for (;;)
        usleep(1000000);
}

/* ---- set-up ---- */

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
    color_pitch = (display_width * 4u + 63u) & ~63u;
    depth_pitch = (display_width * 4u + 63u) & ~63u;
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
    return cellGcmAddressToOffset(local_align(64, depth_pitch * display_height), &depth_offset) == 0;
}

static int attr(CGprogram p, const char *name, int fallback)
{
    CGparameter a = cellGcmCgGetNamedParameter(p, name);
    return a ? (int)cellGcmCgGetParameterResource(p, a) - CG_ATTR0 : fallback;
}

static int init_programs(void)
{
    const void *vps[VP_COUNT] = {vp_offset_vpo, vp_scale_vpo};
    const char *vpc[VP_COUNT] = {"offset", "xform"};
    const void *fps[FP_COUNT] = {fp_color_fpo, fp_tex_fpo, fp_texmod_fpo};
    for (int i = 0; i < VP_COUNT; i++) {
        uint32_t size = 0;
        vp_info *v = &g_vp[i];
        v->prog = (CGprogram)vps[i];
        cellGcmCgInitProgram(v->prog);
        cellGcmCgGetUCode(v->prog, &v->ucode, &size);
        v->constant = cellGcmCgGetNamedParameter(v->prog, vpc[i]);
        v->pos = attr(v->prog, "in_position", 0);
        v->col = attr(v->prog, "in_color", 3);
        v->uv = attr(v->prog, "in_uv", 8);
        if (!v->ucode || !size || !v->constant)
            return 0;
    }
    for (int i = 0; i < FP_COUNT; i++) {
        void *blob = NULL;
        uint32_t size = 0;
        fp_info *f = &g_fp[i];
        f->prog = (CGprogram)fps[i];
        cellGcmCgInitProgram(f->prog);
        cellGcmCgGetUCode(f->prog, &blob, &size);
        if (!blob || !size)
            return 0;
        void *dst = local_align(64, size);
        memcpy(dst, blob, size);
        if (cellGcmAddressToOffset(dst, &f->offset) != 0)
            return 0;
    }
    return 1;
}

/* Three linear 64 x 64 textures in local memory, one per format, a checkerboard each. */
static int init_textures(void)
{
    static const uint32_t bpp[TEX_COUNT] = {4, 2, 1};
    for (int t = 0; t < TEX_COUNT; t++) {
        uint32_t pitch = TEX_SIZE * bpp[t];
        uint8_t *p = local_align(128, pitch * TEX_SIZE);
        for (uint32_t y = 0; y < TEX_SIZE; y++)
            for (uint32_t x = 0; x < TEX_SIZE; x++) {
                int on = ((x >> 3) ^ (y >> 3)) & 1;
                uint8_t *px = p + y * pitch + x * bpp[t];
                if (t == TEX_A8R8G8B8)
                    *(uint32_t *)(void *)px = on ? 0xffe0c040u : 0xff2040a0u;
                else if (t == TEX_R5G6B5)
                    *(uint16_t *)(void *)px = on ? 0xf800u : 0x07e0u;
                else
                    *px = on ? 0xf0u : 0x30u;
            }
        if (cellGcmAddressToOffset(p, &g_tex_offset[t]) != 0)
            return 0;
    }
    return 1;
}

static int init_geometry(void)
{
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    g_vb = local_align(64, 4 * sizeof(vertex_t));
    const float q[4][2] = {{-0.18f, -0.18f}, {0.18f, -0.18f}, {0.18f, 0.18f}, {-0.18f, 0.18f}};
    for (int i = 0; i < 4; i++) {
        g_vb[i].pos[0] = q[i][0];
        g_vb[i].pos[1] = q[i][1];
        g_vb[i].color[0] = i == 1 ? 1.0f : 0.3f;
        g_vb[i].color[1] = i == 2 ? 1.0f : 0.5f;
        g_vb[i].color[2] = i == 3 ? 1.0f : 0.7f;
        g_vb[i].color[3] = 0.75f;
        g_vb[i].uv[0] = (i == 1 || i == 2) ? 1.0f : 0.0f;
        g_vb[i].uv[1] = i >= 2 ? 1.0f : 0.0f;
    }
    uint16_t *ib = local_align(64, sizeof idx);
    memcpy(ib, idx, sizeof idx);
    return cellGcmAddressToOffset(g_vb, &g_vb_offset) == 0 && cellGcmAddressToOffset(ib, &g_ib_offset) == 0;
}

/* ---- per draw ---- */

static void bind_vp(int i, float a, float b, float c, float d)
{
    const vp_info *v = &g_vp[i];
    float k[4] = {a, b, c, d};
    cellGcmSetVertexProgram(g_ctx, v->prog, v->ucode);
    cellGcmSetVertexProgramParameter(g_ctx, v->constant, k);
    cellGcmSetVertexDataArray(g_ctx, (uint8_t)v->pos, 0, (uint8_t)sizeof(vertex_t), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_LOCAL, g_vb_offset + (uint32_t)offsetof(vertex_t, pos));
    cellGcmSetVertexDataArray(g_ctx, (uint8_t)v->col, 0, (uint8_t)sizeof(vertex_t), 4, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_LOCAL, g_vb_offset + (uint32_t)offsetof(vertex_t, color));
    cellGcmSetVertexDataArray(g_ctx, (uint8_t)v->uv, 0, (uint8_t)sizeof(vertex_t), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_LOCAL, g_vb_offset + (uint32_t)offsetof(vertex_t, uv));
    cellGcmSetVertexAttribInputMask(g_ctx, (uint16_t)((1u << v->pos) | (1u << v->col) | (1u << v->uv)));
    cellGcmSetVertexAttribOutputMask(g_ctx, GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | GCM_ATTRIB_OUTPUT_MASK_TEX0);
}

static void bind_texture(int t)
{
    static const uint32_t fmt[TEX_COUNT] = {CELL_GCM_TEXTURE_A8R8G8B8, CELL_GCM_TEXTURE_R5G6B5, CELL_GCM_TEXTURE_B8};
    static const uint32_t bpp[TEX_COUNT] = {4, 2, 1};
    CellGcmTexture tex;
    memset(&tex, 0, sizeof tex);
    tex.format = fmt[t] | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
    tex.mipmap = 1;
    tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
    tex.cubemap = CELL_GCM_FALSE;
    if (t == TEX_B8) /* one channel: every output from it */
        tex.remap = (CELL_GCM_TEXTURE_REMAP_REMAP << 8) | (CELL_GCM_TEXTURE_REMAP_REMAP << 10) |
                    (CELL_GCM_TEXTURE_REMAP_REMAP << 12) | (CELL_GCM_TEXTURE_REMAP_REMAP << 14) |
                    (CELL_GCM_TEXTURE_REMAP_FROM_B << 0) | (CELL_GCM_TEXTURE_REMAP_FROM_B << 2) |
                    (CELL_GCM_TEXTURE_REMAP_FROM_B << 4) | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
    else
        tex.remap = (CELL_GCM_TEXTURE_REMAP_REMAP << 8) | (CELL_GCM_TEXTURE_REMAP_REMAP << 10) |
                    (CELL_GCM_TEXTURE_REMAP_REMAP << 12) | (CELL_GCM_TEXTURE_REMAP_REMAP << 14) |
                    (CELL_GCM_TEXTURE_REMAP_FROM_A << 0) | (CELL_GCM_TEXTURE_REMAP_FROM_R << 2) |
                    (CELL_GCM_TEXTURE_REMAP_FROM_G << 4) | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
    tex.width = TEX_SIZE;
    tex.height = TEX_SIZE;
    tex.depth = 1;
    tex.location = CELL_GCM_LOCATION_LOCAL;
    tex.pitch = TEX_SIZE * bpp[t];
    tex.offset = g_tex_offset[t];
    cellGcmSetTexture(g_ctx, 0, &tex);
    cellGcmSetTextureControl(g_ctx, 0, CELL_GCM_TRUE, 0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
    cellGcmSetTextureFilter(g_ctx, 0, 0, CELL_GCM_TEXTURE_LINEAR, CELL_GCM_TEXTURE_LINEAR,
                            CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    cellGcmSetTextureAddress(g_ctx, 0, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                             CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
    cellGcmSetInvalidateTextureCache(g_ctx, CELL_GCM_INVALIDATE_TEXTURE);
}

static void fixed_function(int depth_test, int cull_back, int blend)
{
    cellGcmSetDepthTestEnable(g_ctx, depth_test ? CELL_GCM_TRUE : CELL_GCM_FALSE);
    cellGcmSetDepthFunc(g_ctx, CELL_GCM_LESS);
    cellGcmSetDepthMask(g_ctx, depth_test ? CELL_GCM_TRUE : CELL_GCM_FALSE);
    cellGcmSetFrontFace(g_ctx, CELL_GCM_CCW);
    cellGcmSetCullFace(g_ctx, CELL_GCM_BACK);
    cellGcmSetCullFaceEnable(g_ctx, cull_back ? CELL_GCM_TRUE : CELL_GCM_FALSE);
    cellGcmSetBlendEnable(g_ctx, blend ? CELL_GCM_TRUE : CELL_GCM_FALSE);
    cellGcmSetBlendFunc(g_ctx, CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA, CELL_GCM_SRC_ALPHA,
                        CELL_GCM_ONE_MINUS_SRC_ALPHA);
    cellGcmSetBlendEquation(g_ctx, CELL_GCM_FUNC_ADD, CELL_GCM_FUNC_ADD);
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
    cellGcmSetLogicOpEnable(g_ctx, CELL_GCM_FALSE);
}

/* The frame's draws, in the order expected/draws.tsv lists them. */
static void render_frame(void)
{
    surface();
    PS3DK_PROF_DRAW_SCOPE_BEGIN("frame");

    PS3DK_PROF_DRAW_LABEL("clear");
    PS3DK_PROF_AUTO_TAG(CLEAR_TAG);
    cellGcmSetClearColor(g_ctx, 0xff102030u);
    cellGcmSetClearDepthStencil(g_ctx, 0xffffff00u);
    cellGcmSetClearSurface(g_ctx, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A |
                                  CELL_GCM_CLEAR_Z | CELL_GCM_CLEAR_S);

    PS3DK_PROF_DRAW_SCOPE_BEGIN("matrix");
    PS3DK_PROF_AUTO_RANGE_BEGIN("matrix");
    for (int k = 0; k < VP_COUNT * FP_COUNT; k++) {
        const matrix_cell *m = &MATRIX[k];
        PS3DK_PROF_DRAW_LABEL(m->label);
        PS3DK_PROF_AUTO_TAG(MATRIX_TAG(k));
        if (m->vp == VP_OFFSET)
            bind_vp(VP_OFFSET, m->x, m->y, 0.0f, 0.0f);
        else
            bind_vp(VP_SCALE, m->x, m->y, 1.0f, 1.0f);
        cellGcmSetFragmentProgram(g_ctx, g_fp[m->fp].prog, g_fp[m->fp].offset);
        if (m->tex != TEX_NONE)
            bind_texture(m->tex);
        fixed_function(m->depth_test, m->cull_back, m->blend);
        cellGcmSetInvalidateVertexCache(g_ctx);
        cellGcmSetDrawArrays(g_ctx, CELL_GCM_PRIMITIVE_QUADS, 0, 4);
    }
    PS3DK_PROF_AUTO_RANGE_END();
    PS3DK_PROF_DRAW_SCOPE_END();

    PS3DK_PROF_DRAW_SCOPE_BEGIN("indexed");
    PS3DK_PROF_DRAW_LABEL("quad");
    PS3DK_PROF_AUTO_TAG(INDEX_TAG);
    bind_vp(VP_SCALE, 0.0f, -0.55f, 1.0f, 0.5f);
    cellGcmSetFragmentProgram(g_ctx, g_fp[FP_COLOR].prog, g_fp[FP_COLOR].offset);
    fixed_function(0, 0, 0);
    cellGcmSetInvalidateVertexCache(g_ctx);
    cellGcmSetDrawIndexArray(g_ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 6, CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16,
                             CELL_GCM_LOCATION_LOCAL, g_ib_offset);
    PS3DK_PROF_DRAW_SCOPE_END();

    PS3DK_PROF_DRAW_SCOPE_BEGIN("instances");
    PS3DK_PROF_DRAW_LABEL("run");
    PS3DK_PROF_AUTO_TAG(INSTANCE_TAG);
    PS3DK_PROF_AUTO_RANGE_BEGIN("instances");
    cellGcmSetFragmentProgram(g_ctx, g_fp[FP_COLOR].prog, g_fp[FP_COLOR].offset);
    fixed_function(0, 0, 1);
    for (int i = 0; i < INSTANCES; i++) {
        bind_vp(VP_OFFSET, -0.84f + 0.24f * (float)i, -0.82f, 0.0f, 0.0f); /* only the constant differs */
        cellGcmSetDrawArrays(g_ctx, CELL_GCM_PRIMITIVE_QUADS, 0, 4);
    }
    PS3DK_PROF_AUTO_RANGE_END();
    PS3DK_PROF_DRAW_SCOPE_END();

    /* the draw modes AUTO times only for the capture: an inline array (the vertex data in the command buffer, the
     * attribute layout of the arrays: position 2, colour 4, uv 2 words per vertex), an inline index array (16-bit
     * indices in the command buffer into the vertex arrays) and an immediate draw (SetDrawBegin, per-vertex
     * attribute writes, SetDrawEnd) with the arrays disabled; the next frame's first bind_vp enables them again */
    PS3DK_PROF_DRAW_SCOPE_BEGIN("special");
    PS3DK_PROF_AUTO_TAG(SPECIAL_TAG);
    cellGcmSetFragmentProgram(g_ctx, g_fp[FP_COLOR].prog, g_fp[FP_COLOR].offset);
    fixed_function(0, 0, 0);
    {
        static const float tri[3][8] = {
            {-0.15f, -0.15f, 1.0f, 0.2f, 0.2f, 1.0f, 0.0f, 0.0f},
            { 0.15f, -0.15f, 0.2f, 1.0f, 0.2f, 1.0f, 1.0f, 0.0f},
            { 0.0f,   0.15f, 0.2f, 0.2f, 1.0f, 1.0f, 0.5f, 1.0f},
        };
        static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        PS3DK_PROF_DRAW_LABEL("inline_array");
        bind_vp(VP_OFFSET, 0.6f, -0.55f, 0.0f, 0.0f);
        cellGcmSetInvalidateVertexCache(g_ctx);
        cellGcmSetDrawInlineArray(g_ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 24, tri);
        PS3DK_PROF_DRAW_LABEL("inline_index");
        bind_vp(VP_OFFSET, 0.85f, -0.55f, 0.0f, 0.0f);
        cellGcmSetInvalidateVertexCache(g_ctx);
        rsxDrawInlineIndexArray16(g_ctx, CELL_GCM_PRIMITIVE_TRIANGLES, 0, 6, idx);
        PS3DK_PROF_DRAW_LABEL("immediate");
        bind_vp(VP_OFFSET, -0.6f, -0.55f, 0.0f, 0.0f);
        for (int a = 0; a < 3; a++) {
            const int attr = a == 0 ? g_vp[VP_OFFSET].pos : a == 1 ? g_vp[VP_OFFSET].col : g_vp[VP_OFFSET].uv;
            cellGcmSetVertexDataArray(g_ctx, (uint8_t)attr, 0, 0, 0, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, 0);
        }
        cellGcmSetDrawBegin(g_ctx, CELL_GCM_PRIMITIVE_TRIANGLES);
        for (int v = 0; v < 3; v++) {
            const float color[4] = {tri[v][2], tri[v][3], tri[v][4], tri[v][5]};
            const float uv[4] = {tri[v][6], tri[v][7], 0.0f, 1.0f};
            const float pos[4] = {tri[v][0], tri[v][1], 0.0f, 1.0f};
            cellGcmSetVertexData4f(g_ctx, (uint8_t)g_vp[VP_OFFSET].col, color);
            cellGcmSetVertexData4f(g_ctx, (uint8_t)g_vp[VP_OFFSET].uv, uv);
            cellGcmSetVertexData4f(g_ctx, (uint8_t)g_vp[VP_OFFSET].pos, pos); /* position last: it emits the vertex */
        }
        cellGcmSetDrawEnd(g_ctx);
    }
    PS3DK_PROF_DRAW_SCOPE_END();

    PS3DK_PROF_DRAW_SCOPE_END();
    PS3DK_PROF_AUTO_TAG(0);
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
    int capture = 0, both = 0;
    uint32_t frames = 0;

    (void)PS3DK_PROF_CAPTURE_WHOLE_RUN(); /* a capture window for the whole run: the request arms at once */
    if (gcm_host_buffer_alloc(HOST_HEAP_SIZE, &host) != 0 || cellGcmInit(CB_SIZE, HOST_HEAP_SIZE, (void *)(uintptr_t)host) != 0) {
        printf("RSXFRAME init failed (host buffer or cellGcmInit)\n");
        return 1;
    }
    cellGcmGetConfiguration(&cfg);
    g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;
    g_ctx = CELL_GCM_CURRENT;
    if (!init_display() || cellSysutilRegisterCallback(0, on_sysutil, NULL) != 0) {
        printf("RSXFRAME init failed (display or callback)\n");
        return 1;
    }
    /* the fence label may hold a value from an earlier app: zero it and read it back before any GPU work */
    volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_FENCE);
    *label = 0;
    __asm__ volatile("sync" ::: "memory");
    if (*label != 0) {
        printf("RSXFRAME label %u did not reset (0x%08x)\n", LABEL_FENCE, (unsigned)*label);
        return 1;
    }
    if (!init_programs() || !init_textures() || !init_geometry()) {
        printf("RSXFRAME init failed (programs, textures or geometry)\n");
        return 1;
    }
    printf("RSXFRAME start frames=%u capture_at=%u draws_per_frame=%u\n", FRAMES, CAPTURE_AT, DRAWS_PER_FRAME);
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
        if (frames == CAPTURE_AT) {
            capture = PS3DK_PROF_RSX_FRAME_CAPTURE("matrix");
            printf("RSXFRAME request frame=%u result=%d\n", frames, capture);
            fflush(stdout);
        }
        if (frames == BOTH_AT) {
            both = ps3dk_prof_rsx_frame_capture("matrix-both", PS3DK_PROF_RSX_FRAME_BOTH | PS3DK_PROF_RSX_FRAME_FP_BYTES);
            printf("RSXFRAME request frame=%u both result=%d\n", frames, both);
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
    }

    /* Exit drain, also after a failure or quit: the last queued flip completes and a FRESH fence passes, each
     * bounded, before anything is sealed or returned; otherwise the RSX is not proven idle. */
    int flip_ok = !g_flip_pending || await(flip_done, 0, DRAIN_US, 0);
    int fence_ok = 0;
    if (flip_ok) {
        g_label_v++;
        cellGcmSetWriteBackEndLabel(g_ctx, LABEL_FENCE, g_label_v);
        cellGcmFlush(g_ctx);
        fence_ok = await(fence_done, g_label_v, DRAIN_US, 0);
    }
    printf("RSXFRAME drain flip=%s fence=%s\n", flip_ok ? "ok" : "timeout", fence_ok ? "ok" : "timeout");
    fflush(stdout);
    if (!flip_ok || !fence_ok)
        recovery_required(flip_ok ? "final-fence" : "final-flip");
    cellSysutilUnregisterCallback(0);
    PS3DK_PROF_AUTO_SEAL(); /* the RSX is idle: the capture (and the RSX frame capture in it) is complete */
    if (g_failed) {
        printf("RSXFRAME EXIT failed at=%s frames=%u capture=%d\n", g_failed, frames, capture);
        return 1;
    }
    printf("RSXFRAME EXIT %s frames=%u capture=%d both=%d\n", g_quit ? "quit" : "clean", frames, capture, both);
    return capture > 0 && both > 0 ? 0 : 2;
}
