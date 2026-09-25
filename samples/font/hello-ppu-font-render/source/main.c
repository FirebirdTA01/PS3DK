/*
 * hello-ppu-font-render - draw text with the system font and show it.
 *
 * The other font samples only check return codes. This one puts glyphs on
 * the screen, through both surfaces the SDK now provides for libfont:
 *
 *   line 1  the canonical cellFont API (cellFontInit, which needs the
 *           stub archive's revision flags, and cellFontInitLibraryFreeType)
 *   line 2  the same font driven through PSL1GHT's font* names
 *           (psl1ght_font.c), after checking that PSL1GHT's pointer-
 *           returning wrappers hand back the canonical library and renderer
 *
 * Glyphs are rendered into an 8-bit coverage surface, composited white on
 * blue into both display buffers, and shown for about three seconds (or
 * until START). FONT_RENDER_OK requires every glyph call to succeed (a
 * failure is printed with its character), each line to have ink inside
 * its band (a smoke check, not a text oracle), no coverage at all outside
 * the bands, and every flip to complete; the verdict is printed last.
 * As an optional artifact that does not affect the verdict, the displayed
 * buffer is written to /dev_hdd0/tmp/hello-ppu-font-render.bmp so the
 * picture can be inspected on the host.
 *
 * libfont hands back each glyph as an image plus a surface position
 * (CellFontImageTransInfo); copying it into the surface is the caller's
 * job, done here by copy_glyph().
 *
 * The -mlp64 build compiles but fails at run time: cellFontInit returns
 * CELL_FONT_ERROR_INVALID_CACHE_BUFFER, because CellFontConfig and the
 * other pointer-bearing font structs are laid out with 8-byte pointers
 * while the firmware reads the 32-bit layout (t_dba7a45b).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>

#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>

#include <cell/sysmodule.h>
#include <cell/font.h>
#include <cell/fontFT.h>
#include <cell/cell_fs.h>

#include "psl1ght_font.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2
#define GCM_LABEL_INDEX 255
#define SHOW_FRAMES 180

#define TEXT_PIXELS 48.0f
#define LINE_COUNT  2

static const char *const line_text[LINE_COUNT] = {
    "cellFont: The quick brown fox 0123456789",
    "PSL1GHT font*: jumps over the lazy dog!",
};

#define BMP_PATH "/dev_hdd0/tmp/hello-ppu-font-render.bmp"

typedef struct {
    uint32_t *ptr;
    uint32_t  offset;
    uint16_t  width;
    uint16_t  height;
    int       id;
} display_buffer;

/* ------------------------------------------------------------------ *
 * RSX bring-up (same pattern as hello-ppu-jpg-dec).
 * ------------------------------------------------------------------ */

static void wait_rsx_idle(gcmContextData *ctx)
{
    u32 label = 1;
    rsxSetWriteBackendLabel(ctx, GCM_LABEL_INDEX, label);
    rsxSetWaitLabel(ctx, GCM_LABEL_INDEX, label);
    label++;
    rsxSetWriteBackendLabel(ctx, GCM_LABEL_INDEX, label);
    rsxFlushBuffer(ctx);
    while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != label)
        usleep(30);
}

/* Waits for the queued flip to complete, for at most FLIP_TIMEOUT_US.
 * Returns 0 on timeout, so a presentation failure cannot hang the sample
 * or be reported as success. */
#define FLIP_TIMEOUT_US 2000000
static int wait_flip(void)
{
    int waited = 0;
    while (gcmGetFlipStatus() != 0) {
        if (waited >= FLIP_TIMEOUT_US)
            return 0;
        usleep(200);
        waited += 200;
    }
    gcmResetFlipStatus();
    return 1;
}

static int do_flip(gcmContextData *ctx, s32 id)
{
    if (gcmSetFlip(ctx, id) == 0) {
        rsxFlushBuffer(ctx);
        gcmSetWaitFlip(ctx);
        return 1;
    }
    return 0;
}

static int make_buffer(display_buffer *b, u16 width, u16 height, int id)
{
    int pitch = (int)(width * sizeof(u32));
    int size  = pitch * height;
    b->ptr = (uint32_t *)rsxMemalign(64, size);
    if (!b->ptr) return 0;
    if (rsxAddressToOffset(b->ptr, &b->offset) != 0) return 0;
    if (gcmSetDisplayBuffer(id, b->offset, pitch, width, height) != 0) return 0;
    b->width  = width;
    b->height = height;
    b->id     = id;
    return 1;
}

static gcmContextData *init_screen(void *host_addr, u32 size, u16 *out_w, u16 *out_h)
{
    gcmContextData    *ctx = NULL;
    videoState         state;
    videoConfiguration vcfg;
    videoResolution    res;

    rsxInit(&ctx, CB_SIZE, size, host_addr);
    if (!ctx) return NULL;

    if (videoGetState(0, 0, &state) != 0 || state.state != 0) return NULL;
    if (videoGetResolution(state.displayMode.resolution, &res) != 0) return NULL;

    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch      = res.width * sizeof(u32);
    vcfg.aspect     = state.displayMode.aspect;

    wait_rsx_idle(ctx);

    if (videoConfigure(0, &vcfg, NULL, 0) != 0) return NULL;
    if (videoGetState(0, 0, &state) != 0) return NULL;

    gcmSetFlipMode(GCM_FLIP_VSYNC);
    gcmResetFlipStatus();

    *out_w = res.width;
    *out_h = res.height;
    return ctx;
}

/* ------------------------------------------------------------------ *
 * libfont set-up through the canonical API.
 * ------------------------------------------------------------------ */

static void *ft_malloc(void *obj, uint32_t size)
{
    (void)obj;
    return malloc(size);
}

static void ft_free(void *obj, void *p)
{
    (void)obj;
    free(p);
}

static void *ft_realloc(void *obj, void *p, uint32_t size)
{
    (void)obj;
    return realloc(p, size);
}

static void *ft_calloc(void *obj, uint32_t n, uint32_t size)
{
    (void)obj;
    return calloc(n, size);
}

typedef struct {
    const CellFontLibrary *lib;
    CellFont               font;
    CellFontRenderer       renderer;
    uint32_t              *file_cache;
} font_state;

static int check(const char *what, int rc)
{
    printf("  %s -> 0x%08x\n", what, (unsigned)rc);
    return rc == CELL_OK;
}

static int font_open(font_state *fs)
{
    CellFontConfig config;
    CellFontLibraryConfigFT ft_config;
    CellFontType type;
    CellFontRendererConfig renderer_config;
    uint64_t revision = 0;

    if (!check("cellSysmoduleLoadModule(FONT)",
               cellSysmoduleLoadModule(CELL_SYSMODULE_FONT))
        || !check("cellSysmoduleLoadModule(FREETYPE)",
                  cellSysmoduleLoadModule(CELL_SYSMODULE_FREETYPE))
        || !check("cellSysmoduleLoadModule(FONTFT)",
                  cellSysmoduleLoadModule(CELL_SYSMODULE_FONTFT)))
        return 0;

    cellFontGetStubRevisionFlags(&revision);
    printf("  cellFontGetStubRevisionFlags -> 0x%llx\n",
           (unsigned long long)revision);

    fs->file_cache = (uint32_t *)memalign(128, 1024 * 1024);
    if (!fs->file_cache) {
        printf("  file cache allocation failed\n");
        return 0;
    }
    CellFontConfig_initialize(&config);
    config.FileCache.buffer = fs->file_cache;
    config.FileCache.size   = 1024 * 1024;
    if (!check("cellFontInit", cellFontInit(&config)))
        return 0;

    CellFontLibraryConfigFT_initialize(&ft_config);
    ft_config.MemoryIF.Malloc  = ft_malloc;
    ft_config.MemoryIF.Free    = ft_free;
    ft_config.MemoryIF.Realloc = ft_realloc;
    ft_config.MemoryIF.Calloc  = ft_calloc;
    if (!check("cellFontInitLibraryFreeType",
               cellFontInitLibraryFreeType(&ft_config, &fs->lib)))
        return 0;

    type.type = CELL_FONT_TYPE_DEFAULT_SANS_SERIF;
    type.map  = CELL_FONT_MAP_UNICODE;
    if (!check("cellFontOpenFontset", cellFontOpenFontset(fs->lib, &type, &fs->font))
        || !check("cellFontSetResolutionDpi",
                  cellFontSetResolutionDpi(&fs->font, 72, 72))
        || !check("cellFontSetScalePixel",
                  cellFontSetScalePixel(&fs->font, TEXT_PIXELS, TEXT_PIXELS)))
        return 0;

    CellFontRendererConfig_initialize(&renderer_config);
    CellFontRendererConfig_setAllocateBuffer(&renderer_config, 64 * 1024,
                                             512 * 1024);
    if (!check("cellFontCreateRenderer",
               cellFontCreateRenderer(fs->lib, &renderer_config, &fs->renderer))
        || !check("cellFontBindRenderer",
                  cellFontBindRenderer(&fs->font, &fs->renderer))
        || !check("cellFontSetupRenderScalePixel",
                  cellFontSetupRenderScalePixel(&fs->font, TEXT_PIXELS, TEXT_PIXELS)))
        return 0;
    return 1;
}

static void font_close(font_state *fs)
{
    cellFontUnbindRenderer(&fs->font);
    cellFontDestroyRenderer(&fs->renderer);
    cellFontCloseFont(&fs->font);
    cellFontEndLibrary(fs->lib);
    cellFontEnd();
    free(fs->file_cache);
}

void copy_glyph(const uint8_t *image, uint32_t image_stride, uint32_t w,
                uint32_t h, uint8_t *dst, uint32_t dst_stride)
{
    if (!image || !dst)
        return;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t a = image[y * image_stride + x];
            if (a > dst[y * dst_stride + x])
                dst[y * dst_stride + x] = a;
        }
}

/* Renders text and stores the final pen position in *pen_end. Every
 * glyph call must succeed, including the ones for spaces (which draw no
 * ink); returns the number of calls that failed, each reported. */
static int canonical_render_text(CellFont *font, uint8_t *buf, int width,
                                 int height, float x, float y,
                                 const char *text, float *pen_end)
{
    CellFontRenderSurface surf;
    CellFontGlyphMetrics metrics;
    CellFontImageTransInfo trans;
    int failed = 0;

    cellFontRenderSurfaceInit(&surf, buf, width, 1, width, height);
    cellFontRenderSurfaceSetScissor(&surf, 0, 0, width, height);
    for (const char *p = text; *p; p++) {
        int rc = cellFontRenderCharGlyphImage(font, (uint8_t)*p, &surf, x, y,
                                              &metrics, &trans);
        if (rc != CELL_OK) {
            printf("  cellFontRenderCharGlyphImage('%c' U+%04X) -> 0x%08x\n",
                   *p, (unsigned)(uint8_t)*p, (unsigned)rc);
            failed++;
            continue;
        }
        /* The glyph is rasterised into the renderer's buffer; TransInfo
         * says where it is and where in the surface it belongs. Copying it
         * across is the caller's job. */
        copy_glyph(trans.Image, trans.imageWidthByte, trans.imageWidth,
                   trans.imageHeight, (uint8_t *)trans.Surface,
                   trans.surfWidthByte);
        x += metrics.Horizontal.advance;
    }
    *pen_end = x;
    return failed;
}

/* ------------------------------------------------------------------ *
 * Coverage checks and output.
 * ------------------------------------------------------------------ */

typedef struct {
    int top, bottom;       /* band the line may occupy */
    int lit, x0, x1, y0, y1;
} line_ink;

static void measure_band(const uint8_t *cov, int width, line_ink *li)
{
    li->lit = 0;
    li->x0 = width; li->x1 = -1; li->y0 = li->bottom; li->y1 = -1;
    for (int y = li->top; y < li->bottom; y++)
        for (int x = 0; x < width; x++)
            if (cov[y * width + x] >= 128) {
                li->lit++;
                if (x < li->x0) li->x0 = x;
                if (x > li->x1) li->x1 = x;
                if (y < li->y0) li->y0 = y;
                if (y > li->y1) li->y1 = y;
            }
}

/* Counts every pixel with any coverage at all: even faint coverage
 * changes the composited colour, so "nothing outside the bands" has to
 * mean nonzero, not merely below the lit threshold. */
static int count_inked(const uint8_t *cov, int width, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < width; x++)
            n += cov[y * width + x] != 0;
    return n;
}

static void composite(display_buffer *dst, const uint8_t *cov)
{
    const uint32_t bg_r = 0x18, bg_g = 0x28, bg_b = 0x50;

    for (int y = 0; y < dst->height; y++)
        for (int x = 0; x < dst->width; x++) {
            uint32_t a = cov[y * dst->width + x];
            uint32_t r = bg_r + ((0xff - bg_r) * a) / 0xff;
            uint32_t g = bg_g + ((0xff - bg_g) * a) / 0xff;
            uint32_t b = bg_b + ((0xff - bg_b) * a) / 0xff;
            dst->ptr[y * dst->width + x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static int write_all(int fd, const void *buf, uint64_t size)
{
    uint64_t written = 0;
    return cellFsWrite(fd, buf, size, &written) == CELL_FS_SUCCEEDED
        && written == size;
}

/* Writes the display buffer as a 24-bit bottom-up BMP. Returns 1 only if
 * every write stored the full requested size and the file closed. */
static int write_bmp(const display_buffer *src)
{
    uint32_t row = ((uint32_t)src->width * 3 + 3) & ~3u;
    uint32_t data = row * src->height;
    uint8_t header[54] = { 'B', 'M' };
    uint8_t *line = (uint8_t *)calloc(1, row);
    int fd, ok = 1;

    put_le32(header + 2, 54 + data);
    put_le32(header + 10, 54);
    put_le32(header + 14, 40);
    put_le32(header + 18, src->width);
    put_le32(header + 22, src->height);
    header[26] = 1;
    header[28] = 24;
    put_le32(header + 34, data);

    if (!line || cellFsOpen(BMP_PATH, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                            &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        free(line);
        return 0;
    }
    ok = write_all(fd, header, sizeof(header));
    for (int y = src->height - 1; ok && y >= 0; y--) {
        const uint32_t *px = src->ptr + y * src->width;
        for (int x = 0; x < src->width; x++) {
            line[x * 3 + 0] = px[x];
            line[x * 3 + 1] = px[x] >> 8;
            line[x * 3 + 2] = px[x] >> 16;
        }
        ok = write_all(fd, line, row);
    }
    if (cellFsClose(fd) != CELL_FS_SUCCEEDED)
        ok = 0;
    free(line);
    return ok;
}

/* Presents the buffers for SHOW_FRAMES frames (START ends early).
 * Returns 0 as soon as a flip fails or does not complete within
 * FLIP_TIMEOUT_US; the caller must then not touch the RSX again. */
static int show_frames(gcmContextData *ctx, display_buffer *buffers)
{
    if (!do_flip(ctx, buffers[MAX_BUFFERS - 1].id) || !wait_flip()) {
        printf("  first flip failed or timed out\n");
        return 0;
    }
    int cur = 0;
    for (int frame = 0; frame < SHOW_FRAMES; frame++) {
        padInfo padinfo;
        int start = 0;
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (padinfo.status[i]) {
                padData paddata = {0};
                ioPadGetData(i, &paddata);
                start |= paddata.BTN_START;
            }
        }
        if (start)
            break;
        if (!do_flip(ctx, buffers[cur].id) || !wait_flip()) {
            printf("  flip failed or timed out at frame %d\n", frame);
            return 0;
        }
        cur = (cur + 1) % MAX_BUFFERS;
    }
    return 1;
}

/* Drains the RSX and frees the display buffers.  rsxFinish waits without
 * a bound, so this is only called once every flip has completed. */
static void release_display(gcmContextData *ctx, display_buffer *buffers)
{
    gcmSetWaitFlip(ctx);
    for (int i = 0; i < MAX_BUFFERS; i++)
        rsxFree(buffers[i].ptr);
    rsxFinish(ctx, 1);
}

static int fail(const char *why)
{
    printf("  %s\nFONT_RENDER_FAIL\n", why);
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-font-render: system font through cellFont and PSL1GHT names\n");

    void *host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!host_addr)
        return fail("host memory allocation failed");
    u16   fb_w = 0, fb_h = 0;
    gcmContextData *ctx = init_screen(host_addr, HOST_SIZE, &fb_w, &fb_h);
    if (!ctx)
        return fail("init_screen failed");
    printf("  RSX up at %ux%u\n", (unsigned)fb_w, (unsigned)fb_h);

    font_state fs;
    memset(&fs, 0, sizeof(fs));
    /* Failure paths leave the RSX alone: rsxFinish waits without a bound,
     * and process exit reclaims the context. */
    if (!font_open(&fs))
        return fail("font set-up failed");

    int ok = psl1ght_check_font(&fs.font, fs.lib, &fs.renderer);

    CellFontHorizontalLayout layout;
    if (!check("cellFontGetHorizontalLayout",
               cellFontGetHorizontalLayout(&fs.font, &layout)))
        return fail("layout query failed");
    printf("  layout: baseLineY=%.1f lineHeight=%.1f\n",
           layout.baseLineY, layout.lineHeight);
    /* The bands are built from the layout, so it must be sane before any
     * index is derived from it. */
    if (!isfinite(layout.lineHeight) || !isfinite(layout.baseLineY)
        || layout.lineHeight <= 0.0f || layout.lineHeight > fb_h / 4
        || layout.baseLineY <= 0.0f || layout.baseLineY > layout.lineHeight)
        return fail("layout values out of range");

    uint8_t *cov = (uint8_t *)calloc(1, (size_t)fb_w * fb_h);
    if (!cov)
        return fail("coverage allocation failed");
    line_ink ink[LINE_COUNT];
    /* A band is every pixel row the line box [top, top + lineHeight)
     * overlaps. Rounding to nearest would drop the last, partly covered
     * row, where descender anti-aliasing legitimately lands. */
    int band = (int)ceilf(layout.lineHeight);
    int first_top = fb_h / 3;
    if (first_top + (2 * LINE_COUNT - 1) * band > fb_h)
        return fail("text bands do not fit the framebuffer");
    for (int i = 0; i < LINE_COUNT; i++) {
        ink[i].top    = first_top + i * band * 2;
        ink[i].bottom = ink[i].top + band;
        float end = 0.0f;
        int failed;
        if (i == 0)
            failed = canonical_render_text(&fs.font, cov, fb_w, fb_h, 64.0f,
                                           (float)ink[i].top, line_text[i], &end);
        else
            failed = psl1ght_render_text(&fs.font, cov, fb_w, fb_h, 64.0f,
                                         (float)ink[i].top, line_text[i], &end);
        measure_band(cov, fb_w, &ink[i]);
        printf("  line %d \"%s\": %d failed glyph calls, pen end x=%.1f, "
               "%d lit pixels in rows %d..%d cols %d..%d\n",
               i + 1, line_text[i], failed, end, ink[i].lit, ink[i].y0,
               ink[i].y1, ink[i].x0, ink[i].x1);
        /* Every glyph must render; the ink thresholds below are only a
         * smoke check that the images landed in the surface, not a text
         * oracle. A line of ~40 glyphs at 48 px lights thousands of pixels
         * and spans most of its pen advance. */
        if (failed != 0)
            ok = 0;
        if (ink[i].lit < 2000 || ink[i].x1 - ink[i].x0 < (int)((end - 64.0f) * 0.8f))
            ok = 0;
    }
    /* No coverage at all may land outside the two bands. */
    int stray = count_inked(cov, fb_w, 0, ink[0].top)
              + count_inked(cov, fb_w, ink[0].bottom, ink[1].top)
              + count_inked(cov, fb_w, ink[1].bottom, fb_h);
    printf("  inked pixels outside the text bands: %d\n", stray);
    if (stray != 0)
        ok = 0;

    ioPadInit(7);
    display_buffer buffers[MAX_BUFFERS];
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!make_buffer(&buffers[i], fb_w, fb_h, i))
            return fail("display buffer set-up failed");
        composite(&buffers[i], cov);
    }
    /* Optional artifact: it does not affect the verdict.  Written before
     * presentation, from the composited buffer contents. */
    if (write_bmp(&buffers[0]))
        printf("  BMP artifact written: %s\n", BMP_PATH);
    else
        printf("  BMP artifact NOT written (optional): %s\n", BMP_PATH);

    printf("  showing for %d frames (START exits early)\n", SHOW_FRAMES);
    if (!show_frames(ctx, buffers)) {
        /* A flip failed or timed out, so the RSX may still hold the
         * buffers and a drain would block without bound.  Leave the RSX
         * state alone and let process exit reclaim it. */
        font_close(&fs);
        free(cov);
        return fail("presentation failed; GPU teardown skipped");
    }

    release_display(ctx, buffers);
    ioPadEnd();
    font_close(&fs);
    free(cov);
    free(host_addr);

    /* The verdict comes last, after rendering, the checks and the display
     * loop have all completed. */
    printf(ok ? "FONT_RENDER_OK\n" : "FONT_RENDER_FAIL\n");
    printf("hello-ppu-font-render: done\n");
    return ok ? 0 : 1;
}
