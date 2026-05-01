/*
 * hello-ppu-png - decode a PNG via cellPngDec and display it via RSX.
 *
 * The decoder lives in the cellPngDec SPRX (libpngdec.sprx); we link
 * against it through our nidgen-generated libpngdec_stub.a.  cellPngDec
 * writes via caller-provided malloc/free callbacks, so passing NULL
 * for the callback fields returns CELL_PNGDEC_ERROR_ARG - Sony's own
 * samples all provide non-null callbacks.
 *
 * Runtime flow:
 *   1. Bring up RSX + video (1280x720 XRGB).
 *   2. cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC).
 *   3. cellPngDec{Create,Open,ReadHeader,SetParameter,DecodeData} on
 *      /dev_hdd0/game/HELPPUPNG1/USRDIR/ps3dk.png when installed as a
 *      .pkg, /app_home/pkg_files/USRDIR/ps3dk.png when launched
 *      directly; falls back to an 8x8 embedded test.png for the
 *      no-file fallback test.
 *   4. Blit the decoded RGBA into both display buffers as XRGB, then
 *      flip until the user presses START.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>

#include <cell/sysmodule.h>
#include <cell/pngdec.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2
#define GCM_LABEL_INDEX 255

extern const uint8_t  test_png[] __attribute__((aligned(4)));
extern const uint32_t test_png_size;

typedef struct {
    uint32_t *ptr;
    uint32_t  offset;
    uint16_t  width;
    uint16_t  height;
    int       id;
} display_buffer;

/* ------------------------------------------------------------------ *
 * cellPngDec callbacks (mandatory - the SPRX has no built-in alloc).
 * ------------------------------------------------------------------ */

static void *png_malloc(uint32_t size, void *arg)
{
    (void)arg;
    return memalign(16, size);
}

static int32_t png_free(void *ptr, void *arg)
{
    (void)arg;
    free(ptr);
    return 0;
}

/* ------------------------------------------------------------------ *
 * RSX bring-up (clone of hello-ppu-rsx-clear's init).
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

static void wait_flip(void) {
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

static int do_flip(gcmContextData *ctx, s32 id) {
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
 * RGBA -> framebuffer blit.  Source pitch may not match destination
 * pitch; we letterbox/center smaller images and clip larger ones.
 * ------------------------------------------------------------------ */

static void blit_rgba_to_xrgb(display_buffer *dst,
                              const uint8_t *rgba, size_t src_stride,
                              uint32_t img_w, uint32_t img_h)
{
    /* Clear to solid dark so letterbox margins are clean. */
    for (int y = 0; y < dst->height; y++)
        for (int x = 0; x < dst->width; x++)
            dst->ptr[y * dst->width + x] = 0xff000000u;

    uint32_t draw_w = img_w < (uint32_t)dst->width  ? img_w : (uint32_t)dst->width;
    uint32_t draw_h = img_h < (uint32_t)dst->height ? img_h : (uint32_t)dst->height;
    uint32_t off_x  = ((uint32_t)dst->width  - draw_w) / 2;
    uint32_t off_y  = ((uint32_t)dst->height - draw_h) / 2;

    for (uint32_t y = 0; y < draw_h; y++) {
        const uint8_t *src_row = rgba + y * src_stride;
        uint32_t      *dst_row = dst->ptr + (off_y + y) * dst->width + off_x;
        for (uint32_t x = 0; x < draw_w; x++) {
            uint8_t r = src_row[x * 4 + 0];
            uint8_t g = src_row[x * 4 + 1];
            uint8_t b = src_row[x * 4 + 2];
            dst_row[x] = ((uint32_t)0xff << 24)
                       | ((uint32_t)r    << 16)
                       | ((uint32_t)g    <<  8)
                       | ((uint32_t)b    <<  0);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Decode path.  Returns malloc'd RGBA buffer + dims on success.
 * ------------------------------------------------------------------ */

static uint8_t *decode_png(uint32_t *out_w, uint32_t *out_h,
                           size_t *out_stride, const char **out_src)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
    if (rc != 0) { printf("  SYSMODULE_PNGDEC load: 0x%08x\n", (unsigned)rc); return NULL; }

    CellPngDecThreadInParam  tin  = {
        .spuThreadEnable   = CELL_PNGDEC_SPU_THREAD_DISABLE,
        .ppuThreadPriority = 512,
        .spuThreadPriority = 200,
        .cbCtrlMallocFunc  = CELL_PNGDEC_CB_EA(png_malloc),
        .cbCtrlMallocArg   = 0,
        .cbCtrlFreeFunc    = CELL_PNGDEC_CB_EA(png_free),
        .cbCtrlFreeArg     = 0,
    };
    CellPngDecThreadOutParam tout = {0};
    CellPngDecMainHandle     main_h = 0;

    rc = cellPngDecCreate(&main_h, &tin, &tout);
    if (rc != 0) { printf("  cellPngDecCreate: 0x%08x\n", (unsigned)rc); goto unload; }

    static const char *const paths[] = {
        /* Install path when loaded from a .pkg.  PSN title-IDs are
         * capped at 9 characters, so pkg.py truncates our 10-char
         * APPID to HELPPUPNG and that is the dir RPCS3 creates under
         * /dev_hdd0/game/. */
        "/dev_hdd0/game/HELPPUPNG/USRDIR/ps3dk.png",
        /* Direct-run fallback: RPCS3 mounts /app_home to the .self's
         * source dir, so pkg_files/USRDIR is visible there. */
        "/app_home/pkg_files/USRDIR/ps3dk.png",
        NULL,
    };
    CellPngDecSubHandle sub_h = 0;
    const char *chosen = NULL;
    for (int i = 0; paths[i]; i++) {
        CellPngDecSrc src = {
            .srcSelect       = CELL_PNGDEC_FILE,
            .fileName        = (uint32_t)(uintptr_t)paths[i],
            .spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE,
        };
        CellPngDecOpnInfo info = {0};
        rc = cellPngDecOpen(main_h, &sub_h, &src, &info);
        printf("  try %s = 0x%08x\n", paths[i], (unsigned)rc);
        if (rc == 0) { chosen = paths[i]; break; }
        /* Close on failure so the decoder state is ready for the next attempt. */
        cellPngDecClose(main_h, sub_h);
    }
    if (!chosen) {
        CellPngDecSrc src = {
            .srcSelect       = CELL_PNGDEC_BUFFER,
            .streamPtr       = (uint32_t)(uintptr_t)test_png,
            .streamSize      = test_png_size,
            .spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE,
        };
        CellPngDecOpnInfo info = {0};
        rc = cellPngDecOpen(main_h, &sub_h, &src, &info);
        if (rc != 0) { printf("  fallback embedded: 0x%08x\n", (unsigned)rc); goto destroy; }
        chosen = "<embedded 8x8 test.png>";
    }
    if (out_src) *out_src = chosen;

    CellPngDecInfo hdr = {0};
    rc = cellPngDecReadHeader(main_h, sub_h, &hdr);
    if (rc != 0) { printf("  ReadHeader: 0x%08x\n", (unsigned)rc); goto close; }

    CellPngDecInParam  ip = {
        .outputMode        = CELL_PNGDEC_TOP_TO_BOTTOM,
        .outputColorSpace  = CELL_PNGDEC_RGBA,
        .outputBitDepth    = 8,
        .outputPackFlag    = CELL_PNGDEC_1BYTE_PER_1PIXEL,
        .outputAlphaSelect = CELL_PNGDEC_STREAM_ALPHA,
    };
    CellPngDecOutParam op = {0};
    rc = cellPngDecSetParameter(main_h, sub_h, &ip, &op);
    if (rc != 0) { printf("  SetParameter: 0x%08x\n", (unsigned)rc); goto close; }

    size_t stride = (size_t)op.outputWidthByte;
    size_t bytes  = stride * op.outputHeight;
    uint8_t *rgba = (uint8_t *)memalign(16, bytes);
    if (!rgba) { printf("  rgba malloc failed (%zu)\n", bytes); goto close; }

    CellPngDecDataCtrlParam dcp  = { .outputBytesPerLine = stride };
    CellPngDecDataOutInfo   dout = {0};
    rc = cellPngDecDecodeData(main_h, sub_h, rgba, &dcp, &dout);
    if (rc != 0 || dout.status != CELL_PNGDEC_DEC_STATUS_FINISH) {
        printf("  DecodeData: 0x%08x status=%u\n", (unsigned)rc, (unsigned)dout.status);
        free(rgba);
        goto close;
    }

    cellPngDecClose(main_h, sub_h);
    cellPngDecDestroy(main_h);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);

    *out_w = op.outputWidth;
    *out_h = op.outputHeight;
    *out_stride = stride;
    return rgba;

close:
    cellPngDecClose(main_h, sub_h);
destroy:
    cellPngDecDestroy(main_h);
unload:
    cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-png: cellPngDec + RSX display\n");

    /* RSX up first. */
    void *host_addr = memalign(1024 * 1024, HOST_SIZE);
    u16   fb_w = 0, fb_h = 0;
    gcmContextData *ctx = init_screen(host_addr, HOST_SIZE, &fb_w, &fb_h);
    if (!ctx) {
        printf("  init_screen failed\n");
        free(host_addr);
        return 1;
    }
    printf("  RSX up at %ux%u\n", (unsigned)fb_w, (unsigned)fb_h);

    /* Decode the PNG. */
    uint32_t img_w = 0, img_h = 0;
    size_t   img_stride = 0;
    const char *src = NULL;
    uint8_t *rgba = decode_png(&img_w, &img_h, &img_stride, &src);
    if (!rgba) {
        printf("  decode failed\n");
        rsxFinish(ctx, 0);
        free(host_addr);
        return 1;
    }
    printf("  decoded %ux%u from %s\n", (unsigned)img_w, (unsigned)img_h, src);

    /* Allocate display buffers + paint the image into both. */
    ioPadInit(7);
    display_buffer buffers[MAX_BUFFERS];
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!make_buffer(&buffers[i], fb_w, fb_h, i)) {
            printf("  make_buffer[%d] failed\n", i);
            free(rgba); rsxFinish(ctx, 0); free(host_addr);
            return 1;
        }
        blit_rgba_to_xrgb(&buffers[i], rgba, img_stride, img_w, img_h);
    }
    do_flip(ctx, MAX_BUFFERS - 1);

    printf("  press START to exit\n");

    int cur = 0;
    volatile int exit_req = 0;
    while (!exit_req) {
        padInfo padinfo;
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (padinfo.status[i]) {
                padData paddata = {0};
                ioPadGetData(i, &paddata);
                /* Detect rising edge — only exit on press, not hold */
                static uint16_t prev_start[MAX_PADS];
                uint16_t cur = paddata.BTN_START;
                if (cur && !prev_start[i]) {
                    exit_req = 1; break;
                }
                prev_start[i] = cur;
            }
        }
        wait_flip();
        do_flip(ctx, buffers[cur].id);
        cur = (cur + 1) % MAX_BUFFERS;
    }

    gcmSetWaitFlip(ctx);
    for (int i = 0; i < MAX_BUFFERS; i++) rsxFree(buffers[i].ptr);
    rsxFinish(ctx, 1);
    ioPadEnd();
    free(rgba);
    free(host_addr);

    printf("hello-ppu-png: done\n");
    return 0;
}
