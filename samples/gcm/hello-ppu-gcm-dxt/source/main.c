/*
 * hello-ppu-gcm-dxt — DXT1, DXT3 and DXT5 textures sampled by the RSX, each next to its uncompressed source.
 *
 * Six panels in three rows (DXT1, DXT3, DXT5): left the 8x8 A8R8G8B8 source, right the compressed payload that
 * tools/dxt-encoder/dxt-example made from it (source/assets.h). The DXT3 and DXT5 rows blend over the background;
 * the DXT1 row ignores alpha in both panels. Frame 0 is copied to main memory behind a fence and every source texel
 * center of every panel is checked against the source pattern; the image and a receipt go to /dev_hdd0/tmp.
 *
 * GPU safety (as the profiler pixel samples): the fence label is reset and read back before any GPU work, every wait
 * is bounded and services system callbacks, a failure stops submitting and goes to the exit drain, and if the drain
 * cannot prove the RSX idle the sample prints RECOVERY_REQUIRED and parks (no command, no return).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <sysutil/video.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <io/pad.h>
#include <ppu-lv2.h>
#include <ppu-types.h>

#include "../../common/gcm_host_buffer.h"
#include "../../common/render_pixels.h"
#include "assets.h"

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define SAMPLE_NAME    "gcm-dxt"
#define CB_SIZE        0x100000
#define HOST_SIZE      (16 * 1024 * 1024)
#define READBACK_AT    (CB_SIZE + 128)   /* frame-0 copy, in the mapped host buffer after the command buffer */
#define FRAMES         600u
#define TEX_W          8u
#define TEX_H          8u
#define PANEL_COUNT    6u
#define COLUMNS        2u
#define ROWS           3u
#define LABEL_FENCE    200u              /* application label range (docs/gcm-labels.md) */
#define WAIT_US        2000000ull
#define SESSION_US     60000000ull
#define DRAIN_US       10000000ull
#define BACKGROUND     0xff202830u
#define CANARY         0xfff100f1u       /* readback fill: an incomplete copy fails the pixel checks */
#define RGB_TOLERANCE  2u
#define DXT5_TOLERANCE 20u               /* this fixture's DXT5 alpha quantization, blended */

typedef struct {
    float pos[2];
    float uv[2];
} vertex_t;

static uint32_t display_width, display_height, color_pitch, depth_pitch;
static uint32_t color_offset[2], depth_offset, cur_buffer;
static uint32_t g_local_heap;
static CGprogram g_vp, g_fp;
static void *g_vp_ucode;
static uint32_t g_fp_offset;
static int g_pos_index, g_uv_index;
static uint32_t g_vb_offset;
static CellGcmContextData *g_ctx;

static unsigned panel_size, panel_x[PANEL_COUNT], panel_y[PANEL_COUNT];
static uint32_t source_argb[TEX_W * TEX_H];
static uint32_t texture_offsets[PANEL_COUNT];
static uint8_t texture_formats[PANEL_COUNT];
static uint32_t texture_pitches[PANEL_COUNT];
static volatile uint32_t *readback;
static uint32_t readback_offset;
static unsigned pixel_checks, pixel_failures;
static char image_path[128], receipt_path[128];

static volatile int g_quit;
static const char *g_failed;
static uint64_t g_start;
static uint32_t g_label_v;
static int g_flip_pending;

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
        printf(SAMPLE_NAME ": FAIL at=%s elapsed_us=%llu\n", where, (unsigned long long)(now_us() - g_start));
        fflush(stdout);
    }
}

static int flip_done(uint32_t v) { (void)v; return cellGcmGetFlipStatus() == 0; }
static int fence_done(uint32_t v) { return cellGcmIsLabelPassed(LABEL_FENCE, v) != 0; }

/* Polls ready() until it holds, limit_us passes or (session_bound) the session deadline passes. */
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

/* A back-end label behind everything submitted so far, flushed and waited for (bounded). */
static int fence(uint64_t limit_us, int session_bound)
{
    g_label_v++;
    cellGcmSetWriteBackEndLabel(g_ctx, LABEL_FENCE, g_label_v);
    cellGcmFlush(g_ctx);
    return await(fence_done, g_label_v, limit_us, session_bound);
}

static void recovery_required(const char *where) __attribute__((noreturn));
static void recovery_required(const char *where)
{
    printf(SAMPLE_NAME ": RECOVERY_REQUIRED at=%s no_return=1\n", where);
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
    if (display_width < 320 || display_height < 240)
        return 0;
    color_pitch = (display_width * 4u + 63u) & ~63u;
    depth_pitch = (display_width * 2u + 63u) & ~63u;
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

static int init_programs(void)
{
    uint32_t size = 0;
    void *blob = NULL;
    g_vp = (CGprogram)vpshader_vpo;
    g_fp = (CGprogram)fpshader_fpo;
    cellGcmCgInitProgram(g_vp);
    cellGcmCgInitProgram(g_fp);
    cellGcmCgGetUCode(g_vp, &g_vp_ucode, &size);
    if (!g_vp_ucode || !size)
        return 0;
    CGparameter pos = cellGcmCgGetNamedParameter(g_vp, "in_position");
    CGparameter uv = cellGcmCgGetNamedParameter(g_vp, "in_texcoord");
    if (!pos || !uv)
        return 0;
    g_pos_index = (int)cellGcmCgGetParameterResource(g_vp, pos) - CG_ATTR0;
    g_uv_index = (int)cellGcmCgGetParameterResource(g_vp, uv) - CG_ATTR0;
    if (g_pos_index < 0 || g_pos_index > 15 || g_uv_index < 0 || g_uv_index > 15)
        return 0;
    size = 0;
    cellGcmCgGetUCode(g_fp, &blob, &size);
    if (!blob || !size)
        return 0;
    void *dst = local_align(64, size);
    memcpy(dst, blob, size);
    return cellGcmAddressToOffset(dst, &g_fp_offset) == 0;
}

/* Panels 0, 2, 4: the uncompressed source; panels 1, 3, 5: its DXT1, DXT3, DXT5 payload. */
static int init_textures(void)
{
    for (unsigned i = 0; i < TEX_W * TEX_H; ++i) {
        const uint8_t *p = sample_rgba + 4 * i; /* R, G, B, A */
        source_argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    for (unsigned panel = 0; panel < PANEL_COUNT; ++panel) {
        const void *bytes = source_argb;
        uint32_t size = sizeof source_argb;
        uint8_t format = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
        uint32_t pitch = TEX_W * 4u;
        if (panel & 1u) {
            /* 8x8 is 2x2 blocks: block order is the same linear or swizzled */
            bytes = panel == 1 ? sample_dxt1 : panel == 3 ? sample_dxt3 : sample_dxt5;
            size = panel == 1 ? sizeof sample_dxt1 : sizeof sample_dxt3;
            format = (panel == 1 ? CELL_GCM_TEXTURE_COMPRESSED_DXT1 : panel == 3 ? CELL_GCM_TEXTURE_COMPRESSED_DXT23
                                                                                 : CELL_GCM_TEXTURE_COMPRESSED_DXT45) |
                     CELL_GCM_TEXTURE_NR;
            pitch = 0;
        }
        void *data = local_align(128, size);
        memcpy(data, bytes, size);
        texture_formats[panel] = format;
        texture_pitches[panel] = pitch;
        if (cellGcmAddressToOffset(data, &texture_offsets[panel]) != 0)
            return 0;
    }
    return 1;
}

static int init_geometry(void)
{
    panel_size = display_width / (COLUMNS + 1u);
    if (panel_size > display_height / (ROWS + 1u))
        panel_size = display_height / (ROWS + 1u);
    panel_size = (panel_size / TEX_W) * TEX_W;
    if (!panel_size)
        return 0;
    unsigned gap_x = (display_width - COLUMNS * panel_size) / (COLUMNS + 1u);
    unsigned gap_y = (display_height - ROWS * panel_size) / (ROWS + 1u);
    vertex_t *vb = (vertex_t *)local_align(128, 4u * PANEL_COUNT * sizeof(vertex_t));
    for (unsigned p = 0; p < PANEL_COUNT; ++p) {
        unsigned x = gap_x + (p % COLUMNS) * (panel_size + gap_x);
        unsigned y = gap_y + (p / COLUMNS) * (panel_size + gap_y);
        panel_x[p] = x;
        panel_y[p] = y;
        float l = 2.0f * x / display_width - 1, r = 2.0f * (x + panel_size) / display_width - 1;
        float t = 1 - 2.0f * y / display_height, b = 1 - 2.0f * (y + panel_size) / display_height;
        vb[4 * p + 0] = (vertex_t){{l, t}, {0, 0}};
        vb[4 * p + 1] = (vertex_t){{r, t}, {1, 0}};
        vb[4 * p + 2] = (vertex_t){{l, b}, {0, 1}};
        vb[4 * p + 3] = (vertex_t){{r, b}, {1, 1}};
        printf("  panel %u at %u,%u size %u format 0x%02x pitch %u\n", p, x, y, panel_size, texture_formats[p],
               texture_pitches[p]);
    }
    return cellGcmAddressToOffset(vb, &g_vb_offset) == 0;
}

static void bind_texture(unsigned panel)
{
    CellGcmTexture t;
    memset(&t, 0, sizeof t);
    t.format = texture_formats[panel];
    t.mipmap = 1;
    t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
    t.cubemap = CELL_GCM_FALSE;
    t.remap = (CELL_GCM_TEXTURE_REMAP_REMAP << 8) | (CELL_GCM_TEXTURE_REMAP_REMAP << 10) |
              (CELL_GCM_TEXTURE_REMAP_REMAP << 12) | (CELL_GCM_TEXTURE_REMAP_REMAP << 14) |
              (CELL_GCM_TEXTURE_REMAP_FROM_A << 0) | (CELL_GCM_TEXTURE_REMAP_FROM_R << 2) |
              (CELL_GCM_TEXTURE_REMAP_FROM_G << 4) | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
    t.width = TEX_W;
    t.height = TEX_H;
    t.depth = 1;
    t.location = CELL_GCM_LOCATION_LOCAL;
    t.pitch = texture_pitches[panel];
    t.offset = texture_offsets[panel];
    cellGcmSetTexture(g_ctx, 0, &t);
    cellGcmSetTextureControl(g_ctx, 0, CELL_GCM_TRUE, 0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
    cellGcmSetTextureFilter(g_ctx, 0, 0, CELL_GCM_TEXTURE_NEAREST, CELL_GCM_TEXTURE_NEAREST,
                            CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    cellGcmSetTextureAddress(g_ctx, 0, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                             CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void set_state(void)
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
    sf.depthFormat = CELL_GCM_SURFACE_Z16;
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
    cellGcmSetDepthTestEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetStencilTestEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetAlphaTestEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetCullFaceEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetLogicOpEnable(g_ctx, CELL_GCM_FALSE);
    cellGcmSetBlendFunc(g_ctx, CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA, CELL_GCM_ONE, CELL_GCM_ZERO);
    cellGcmSetBlendEquation(g_ctx, CELL_GCM_FUNC_ADD, CELL_GCM_FUNC_ADD);
    cellGcmSetVertexProgram(g_ctx, g_vp, g_vp_ucode);
    cellGcmSetVertexDataArray(g_ctx, (uint8_t)g_pos_index, 0, sizeof(vertex_t), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_LOCAL, g_vb_offset + offsetof(vertex_t, pos));
    cellGcmSetVertexDataArray(g_ctx, (uint8_t)g_uv_index, 0, sizeof(vertex_t), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_LOCAL, g_vb_offset + offsetof(vertex_t, uv));
    cellGcmSetVertexAttribInputMask(g_ctx, (uint16_t)((1u << g_pos_index) | (1u << g_uv_index)));
    cellGcmSetFragmentProgram(g_ctx, g_fp, g_fp_offset);
}

static void render_frame(void)
{
    set_state();
    cellGcmSetClearColor(g_ctx, BACKGROUND);
    cellGcmSetClearSurface(g_ctx, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
    for (unsigned panel = 0; panel < PANEL_COUNT; ++panel) {
        bind_texture(panel);
        /* rows 2 and 3 (DXT3, DXT5) blend their alpha over the background */
        cellGcmSetBlendEnable(g_ctx, panel >= 2 ? CELL_GCM_TRUE : CELL_GCM_FALSE);
        cellGcmSetDrawArrays(g_ctx, CELL_GCM_PRIMITIVE_TRIANGLE_STRIP, 4 * panel, 4);
    }
}

/* Copies the rendered (not yet flipped) buffer to main memory: fence the draws, transfer, fence the transfer. */
static int read_back(void)
{
    if (!fence(WAIT_US, 1))
        return 0;
    cellGcmSetTransferImage(g_ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN, readback_offset, color_pitch, 0, 0,
                            color_offset[cur_buffer], color_pitch, 0, 0, display_width, display_height, 4);
    return fence(WAIT_US, 1);
}

static uint32_t expected_pixel(unsigned panel, unsigned i)
{
    return panel >= 2 ? sample_blend_rgb(source_argb[i], BACKGROUND) : source_argb[i];
}

static void inspect_pixels(void)
{
    unsigned scale = panel_size / TEX_W;
    for (unsigned p = 0; p < PANEL_COUNT; ++p) {
        uint32_t expected[TEX_W * TEX_H];
        unsigned tolerance = p == 5 ? DXT5_TOLERANCE : RGB_TOLERANCE;
        for (unsigned i = 0; i < TEX_W * TEX_H; ++i)
            expected[i] = expected_pixel(p, i);
        int errors = sample_check_panel(readback, display_width, display_height, color_pitch / 4, panel_x[p],
                                        panel_y[p], scale, TEX_W, TEX_H, expected, tolerance);
        if (errors < 0)
            ++pixel_failures;
        else {
            pixel_checks += TEX_W * TEX_H;
            pixel_failures += (unsigned)errors;
        }
        printf("  panel %u: %u pixels, %d mismatches, tolerance %u/255\n", p, TEX_W * TEX_H, errors, tolerance);
    }
}

static int close_file(FILE *f, int ok)
{
    if (fflush(f) != 0)
        ok = 0;
    if (fsync(fileno(f)) != 0)
        ok = 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok;
}

static int save_image(void)
{
    char tmp[144];
    snprintf(tmp, sizeof tmp, "%s.tmp", image_path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    int ok = fprintf(f, "P6\n%u %u\n255\n", display_width, display_height) > 0;
    uint8_t *row = malloc(display_width * 3u);
    if (!row)
        ok = 0;
    for (unsigned y = 0; ok && y < display_height; ++y) {
        for (unsigned x = 0; x < display_width; ++x) {
            uint32_t c = readback[y * (color_pitch / 4) + x];
            row[3 * x] = (uint8_t)(c >> 16);
            row[3 * x + 1] = (uint8_t)(c >> 8);
            row[3 * x + 2] = (uint8_t)c;
        }
        if (fwrite(row, 3, display_width, f) != display_width)
            ok = 0;
    }
    free(row);
    ok = close_file(f, ok);
    if (ok && rename(tmp, image_path) != 0)
        ok = 0;
    if (!ok)
        unlink(tmp);
    return ok;
}

static int write_receipt(uint64_t run_id, unsigned frames, int pass)
{
    char tmp[144];
    snprintf(tmp, sizeof tmp, "%s.tmp", receipt_path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    int ok = fprintf(f, "sample=%s run=%016llx frames=%u checks=%u mismatches=%u image=%s result=%s receipt=complete\n",
                     SAMPLE_NAME, (unsigned long long)run_id, frames, pixel_checks, pixel_failures, image_path,
                     pass ? "PASS" : "FAIL") > 0;
    ok = close_file(f, ok);
    if (ok && rename(tmp, receipt_path) != 0)
        ok = 0;
    if (!ok)
        unlink(tmp);
    return ok;
}

static void poll_start(void)
{
    static uint16_t prev_start[MAX_PADS];
    padInfo info;
    if (ioPadGetInfo(&info) != 0)
        return;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!info.status[i])
            continue;
        padData data;
        memset(&data, 0, sizeof data);
        if (ioPadGetData(i, &data) != 0)
            continue;
        uint16_t cur = data.BTN_START;
        if (cur && !prev_start[i])
            g_quit = 1; /* press, not hold */
        prev_start[i] = cur;
    }
}

int main(void)
{
    sys_addr_t host = 0;
    CellGcmConfig cfg;
    unsigned frames = 0;
    int saved = 0;

    printf(SAMPLE_NAME ": DXT1/DXT3/DXT5 texture panels\n");
    uint64_t run_id = now_us();
    snprintf(image_path, sizeof image_path, "/dev_hdd0/tmp/%s-%016llx.ppm", SAMPLE_NAME, (unsigned long long)run_id);
    snprintf(receipt_path, sizeof receipt_path, "/dev_hdd0/tmp/%s-%016llx.txt", SAMPLE_NAME,
             (unsigned long long)run_id);

    /* LV2 pages, not the sbrk heap (gcm_host_buffer.h); kept for the life of the process. */
    if (gcm_host_buffer_alloc(HOST_SIZE, &host) != 0 ||
        cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host) != 0) {
        printf(SAMPLE_NAME ": init failed (host buffer or cellGcmInit)\n");
        return 1;
    }
    cellGcmGetConfiguration(&cfg);
    g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;
    g_ctx = CELL_GCM_CURRENT;
    if (!init_display() || cellSysutilRegisterCallback(0, on_sysutil, NULL) != 0) {
        printf(SAMPLE_NAME ": init failed (display or callback)\n");
        return 1;
    }
    /* A label keeps its value across applications: zero the fence and read it back before any GPU work. */
    volatile uint32_t *label = (volatile uint32_t *)cellGcmGetLabelAddress(LABEL_FENCE);
    *label = 0;
    __asm__ volatile("sync" ::: "memory");
    if (*label != 0) {
        printf(SAMPLE_NAME ": label %u did not reset (0x%08x)\n", LABEL_FENCE, (unsigned)*label);
        cellSysutilUnregisterCallback(0);
        return 1;
    }
    if ((uint64_t)color_pitch * display_height > HOST_SIZE - READBACK_AT) {
        printf(SAMPLE_NAME ": readback does not fit the host buffer\n");
        cellSysutilUnregisterCallback(0);
        return 1;
    }
    readback = (volatile uint32_t *)((uintptr_t)host + READBACK_AT);
    if (cellGcmAddressToOffset((void *)readback, &readback_offset) != 0 || !init_programs() || !init_textures() ||
        !init_geometry()) {
        printf(SAMPLE_NAME ": init failed (readback, programs, textures or geometry)\n");
        cellSysutilUnregisterCallback(0);
        return 1;
    }
    for (unsigned i = 0; i < (color_pitch / 4) * display_height; ++i)
        readback[i] = CANARY;
    ioPadInit(7);
    printf("  RSX up at %ux%u, %u frames\n", display_width, display_height, FRAMES);
    fflush(stdout);

    g_start = now_us();
    cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
    /* the VP literal pool once, before the first draw; textures were written by the PPU */
    cellGcmCgUploadInternalConsts(g_ctx, g_vp);
    cellGcmSetInvalidateTextureCache(g_ctx, CELL_GCM_INVALIDATE_TEXTURE);
    for (frames = 0; frames < FRAMES && !g_failed && !g_quit; frames++) {
        if (cellSysutilCheckCallback() != 0) {
            fail("sysutil-check");
            break;
        }
        if (now_us() - g_start >= SESSION_US) {
            fail("session-deadline");
            break;
        }
        poll_start();
        if (g_quit)
            break;
        render_frame();
        if (frames == 0) {
            if (!read_back()) {
                fail("readback-fence");
                break;
            }
            inspect_pixels();
            saved = save_image();
            if (!saved)
                fail("save-image");
        }
        cellGcmResetFlipStatus();
        if (cellGcmSetFlip(g_ctx, (uint8_t)cur_buffer) != 0) {
            fail("submit-flip");
            break;
        }
        cellGcmFlush(g_ctx);
        g_flip_pending = 1;
        cur_buffer ^= 1u;
        if (!await(flip_done, 0, WAIT_US, 1)) {
            fail("flip-wait");
            break;
        }
        g_flip_pending = 0;
    }

    /* Drain: the last flip, then a fence behind everything submitted. */
    int flip_ok = !g_flip_pending || await(flip_done, 0, DRAIN_US, 0);
    int fence_ok = flip_ok && fence(DRAIN_US, 0);
    printf("  drain flip=%s fence=%s\n", flip_ok ? "ok" : "timeout", fence_ok ? "ok" : "timeout");
    fflush(stdout);
    if (!flip_ok || !fence_ok)
        recovery_required(flip_ok ? "final-fence" : "final-flip");
    ioPadEnd();
    cellSysutilUnregisterCallback(0);

    int ok = !g_failed && frames == FRAMES && saved && pixel_checks && !pixel_failures;
    if (!write_receipt(run_id, frames, ok))
        ok = 0;
    printf(SAMPLE_NAME ": %s frames=%u pixel_checks=%u pixel_failures=%u image=%s receipt=%s%s%s\n",
           ok ? "PASS" : "FAIL", frames, pixel_checks, pixel_failures, image_path, receipt_path,
           g_quit ? " quit=1" : "", g_failed ? " failed=1" : "");
    return ok ? 0 : 1;
}
