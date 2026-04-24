/*
 * hello-ppu-jpg-dec - decode a JPEG via cellJpgDec and display it via RSX.
 *
 * Drives the full libjpgdec lifecycle through our nidgen-generated
 * libjpgdec_stub.a.  Matches hello-ppu-png's structure; only the
 * Jpg-vs-Png differences (struct field names, CELL_JPG output colour
 * space, CellJpgDecInParam.method / outputMode shape) appear in the
 * decode_jpg() path.
 *
 * Flow:
 *   1. RSX up + 1280x720 XRGB framebuffers.
 *   2. cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC).
 *   3. cellJpgDec{Create, Open (buffer), ReadHeader, SetParameter,
 *      DecodeData} on an embedded test.jpg.
 *   4. Blit decoded ARGB into both display buffers, flip until the
 *      user presses START.
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
#include <cell/codec/jpgdec.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2
#define GCM_LABEL_INDEX 255

extern const uint8_t  test_jpg[] __attribute__((aligned(4)));
extern const uint32_t test_jpg_size;

typedef struct {
    uint32_t *ptr;
    uint32_t  offset;
    uint16_t  width;
    uint16_t  height;
    int       id;
} display_buffer;

/* ------------------------------------------------------------------ *
 * cellJpgDec callbacks (mandatory - the SPRX has no built-in alloc).
 * ------------------------------------------------------------------ */

static void *jpg_malloc(uint32_t size, void *arg)
{
    (void)arg;
    return memalign(16, size);
}

static int32_t jpg_free(void *ptr, void *arg)
{
    (void)arg;
    free(ptr);
    return 0;
}

/* ------------------------------------------------------------------ *
 * RSX bring-up (same pattern as hello-ppu-png / hello-ppu-rsx-clear).
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
 * cellJpgDec outputs ARGB8888 with outputComponents=4 when we ask for
 * CELL_JPG_ARGB - alpha byte first, then R, G, B.  The XRGB framebuffer
 * uses alpha-ignored 0xff top byte, so reading output bytes as
 * [a, r, g, b] and emitting (0xff<<24)|(r<<16)|(g<<8)|b works.
 * ------------------------------------------------------------------ */

static void blit_argb_to_xrgb(display_buffer *dst,
                              const uint8_t *argb, size_t src_stride,
                              uint32_t img_w, uint32_t img_h)
{
    for (int y = 0; y < dst->height; y++)
        for (int x = 0; x < dst->width; x++)
            dst->ptr[y * dst->width + x] = 0xff000000u;

    uint32_t draw_w = img_w < (uint32_t)dst->width  ? img_w : (uint32_t)dst->width;
    uint32_t draw_h = img_h < (uint32_t)dst->height ? img_h : (uint32_t)dst->height;
    uint32_t off_x  = ((uint32_t)dst->width  - draw_w) / 2;
    uint32_t off_y  = ((uint32_t)dst->height - draw_h) / 2;

    for (uint32_t y = 0; y < draw_h; y++) {
        const uint8_t *src_row = argb + y * src_stride;
        uint32_t      *dst_row = dst->ptr + (off_y + y) * dst->width + off_x;
        for (uint32_t x = 0; x < draw_w; x++) {
            uint8_t r = src_row[x * 4 + 1];
            uint8_t g = src_row[x * 4 + 2];
            uint8_t b = src_row[x * 4 + 3];
            dst_row[x] = ((uint32_t)0xff << 24)
                       | ((uint32_t)r    << 16)
                       | ((uint32_t)g    <<  8)
                       | ((uint32_t)b    <<  0);
        }
    }
}

/* ------------------------------------------------------------------ *
 * JPEG decode. Returns malloc'd ARGB buffer + dims on success.
 * ------------------------------------------------------------------ */

static uint8_t *decode_jpg(uint32_t *out_w, uint32_t *out_h, size_t *out_stride)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC);
    if (rc != 0) { printf("  SYSMODULE_JPGDEC load: 0x%08x\n", (unsigned)rc); return NULL; }

    CellJpgDecThreadInParam tin = {
        .spuThreadEnable   = CELL_JPGDEC_SPU_THREAD_DISABLE,
        .ppuThreadPriority = 512,
        .spuThreadPriority = 200,
        .cbCtrlMallocFunc  = jpg_malloc,
        .cbCtrlMallocArg   = NULL,
        .cbCtrlFreeFunc    = jpg_free,
        .cbCtrlFreeArg     = NULL,
    };
    CellJpgDecThreadOutParam tout = {0};
    CellJpgDecMainHandle     main_h = 0;

    rc = cellJpgDecCreate(&main_h, &tin, &tout);
    if (rc != 0) { printf("  cellJpgDecCreate: 0x%08x\n", (unsigned)rc); goto unload; }

    CellJpgDecSrc src = {
        .srcSelect       = CELL_JPGDEC_BUFFER,
        .streamPtr       = (void *)test_jpg,
        .streamSize      = test_jpg_size,
        .spuThreadEnable = CELL_JPGDEC_SPU_THREAD_DISABLE,
    };
    CellJpgDecOpnInfo   opn  = {0};
    CellJpgDecSubHandle sub_h = 0;
    rc = cellJpgDecOpen(main_h, &sub_h, &src, &opn);
    if (rc != 0) { printf("  cellJpgDecOpen: 0x%08x\n", (unsigned)rc); goto destroy; }

    CellJpgDecInfo hdr = {0};
    rc = cellJpgDecReadHeader(main_h, sub_h, &hdr);
    if (rc != 0) { printf("  ReadHeader: 0x%08x\n", (unsigned)rc); goto close; }
    printf("  header %ux%u components=%u\n",
           (unsigned)hdr.imageWidth, (unsigned)hdr.imageHeight,
           (unsigned)hdr.numComponents);

    CellJpgDecInParam ip = {
        .commandPtr       = NULL,
        .method           = CELL_JPGDEC_FAST,
        .outputMode       = CELL_JPGDEC_TOP_TO_BOTTOM,
        .outputColorSpace = CELL_JPG_ARGB,
        .downScale        = 1,
        .outputColorAlpha = 0xff,
    };
    CellJpgDecOutParam op = {0};
    rc = cellJpgDecSetParameter(main_h, sub_h, &ip, &op);
    if (rc != 0) { printf("  SetParameter: 0x%08x\n", (unsigned)rc); goto close; }

    size_t stride = (size_t)op.outputWidthByte;
    size_t bytes  = stride * op.outputHeight;
    uint8_t *argb = (uint8_t *)memalign(16, bytes);
    if (!argb) { printf("  argb malloc failed (%zu)\n", bytes); goto close; }

    CellJpgDecDataCtrlParam dcp  = { .outputBytesPerLine = stride };
    CellJpgDecDataOutInfo   dout = {0};
    rc = cellJpgDecDecodeData(main_h, sub_h, argb, &dcp, &dout);
    if (rc != 0 || dout.status != CELL_JPGDEC_DEC_STATUS_FINISH) {
        printf("  DecodeData: 0x%08x status=%u\n", (unsigned)rc, (unsigned)dout.status);
        free(argb);
        goto close;
    }

    cellJpgDecClose(main_h, sub_h);
    cellJpgDecDestroy(main_h);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_JPGDEC);

    *out_w      = op.outputWidth;
    *out_h      = op.outputHeight;
    *out_stride = stride;
    return argb;

close:
    cellJpgDecClose(main_h, sub_h);
destroy:
    cellJpgDecDestroy(main_h);
unload:
    cellSysmoduleUnloadModule(CELL_SYSMODULE_JPGDEC);
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("hello-ppu-jpg-dec: cellJpgDec + RSX display\n");

    void *host_addr = memalign(1024 * 1024, HOST_SIZE);
    u16   fb_w = 0, fb_h = 0;
    gcmContextData *ctx = init_screen(host_addr, HOST_SIZE, &fb_w, &fb_h);
    if (!ctx) {
        printf("  init_screen failed\n");
        free(host_addr);
        return 1;
    }
    printf("  RSX up at %ux%u\n", (unsigned)fb_w, (unsigned)fb_h);

    uint32_t img_w = 0, img_h = 0;
    size_t   img_stride = 0;
    uint8_t *argb = decode_jpg(&img_w, &img_h, &img_stride);
    if (!argb) {
        printf("  decode failed\n");
        rsxFinish(ctx, 0);
        free(host_addr);
        return 1;
    }
    printf("  decoded %ux%u (stride=%zu)\n",
           (unsigned)img_w, (unsigned)img_h, img_stride);

    ioPadInit(7);
    display_buffer buffers[MAX_BUFFERS];
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!make_buffer(&buffers[i], fb_w, fb_h, i)) {
            printf("  make_buffer[%d] failed\n", i);
            free(argb); rsxFinish(ctx, 0); free(host_addr);
            return 1;
        }
        blit_argb_to_xrgb(&buffers[i], argb, img_stride, img_w, img_h);
    }
    do_flip(ctx, MAX_BUFFERS - 1);

    printf("  press START to exit\n");

    int cur = 0;
    volatile int exit_req = 0;
    while (!exit_req) {
        padInfo padinfo;
        padData paddata;
        ioPadGetInfo(&padinfo);
        for (int i = 0; i < MAX_PADS; i++) {
            if (padinfo.status[i]) {
                ioPadGetData(i, &paddata);
                if (paddata.BTN_START) { exit_req = 1; break; }
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
    free(argb);
    free(host_addr);

    printf("hello-ppu-jpg-dec: done\n");
    return 0;
}
