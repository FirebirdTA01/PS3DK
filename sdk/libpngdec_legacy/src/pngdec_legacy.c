/* sdk/libpngdec_legacy/src/pngdec_legacy.c
 *
 * PSL1GHT libpngdec legacy convenience API: pngLoadFromFile and pngLoadFromBuffer.
 *
 * All other PSL1GHT pngDec* names are nidgen aliases of the reference cellPngDec*
 * exports (libpngdec_stub.yaml).
 *
 * This implementation wraps the reference cellPngDec API cleanly without
 * depending on PSL1GHT's original implementation.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cell/pngdec.h>

typedef struct _png_data {
    void *bmp_out;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
} pngData;

int32_t pngLoadFromFile(const char *filename, pngData *out);
int32_t pngLoadFromBuffer(const void *buffer, uint32_t size, pngData *out);

static void *png_cb_malloc(uint32_t size, void *arg)
{
    (void)arg;
    return malloc(size);
}

static int32_t png_cb_free(void *ptr, void *arg)
{
    (void)arg;
    free(ptr);
    return 0;
}

static int32_t decode_png(const CellPngDecSrc *src, pngData *out)
{
    if (src == NULL || out == NULL)
        return CELL_PNGDEC_ERROR_ARG;

    out->bmp_out = NULL;
    out->pitch = 0;
    out->width = 0;
    out->height = 0;

    CellPngDecThreadInParam thread_in;
    memset(&thread_in, 0, sizeof(thread_in));
    thread_in.spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;
    thread_in.ppuThreadPriority = 512;
    thread_in.spuThreadPriority = 200;
    thread_in.cbCtrlMallocFunc = CELL_PNGDEC_CB_EA(png_cb_malloc);
    thread_in.cbCtrlFreeFunc = CELL_PNGDEC_CB_EA(png_cb_free);

    CellPngDecThreadOutParam thread_out;
    memset(&thread_out, 0, sizeof(thread_out));

    CellPngDecMainHandle main_handle = 0;
    int32_t ret = cellPngDecCreate(&main_handle, &thread_in, &thread_out);
    if (ret != 0)
        return ret;

    CellPngDecSubHandle sub_handle = 0;
    CellPngDecOpnInfo open_info;
    memset(&open_info, 0, sizeof(open_info));

    ret = cellPngDecOpen(main_handle, &sub_handle, src, &open_info);
    if (ret != 0) {
        cellPngDecDestroy(main_handle);
        return ret;
    }

    CellPngDecInfo dec_info;
    memset(&dec_info, 0, sizeof(dec_info));
    ret = cellPngDecReadHeader(main_handle, sub_handle, &dec_info);
    if (ret != 0)
        goto cleanup_close;

    CellPngDecInParam in_param;
    memset(&in_param, 0, sizeof(in_param));
    in_param.commandPtr = 0;
    in_param.outputMode = CELL_PNGDEC_TOP_TO_BOTTOM;
    in_param.outputColorSpace = CELL_PNGDEC_ARGB;
    in_param.outputBitDepth = 8;
    in_param.outputPackFlag = CELL_PNGDEC_1BYTE_PER_1PIXEL;
    if (dec_info.colorSpace == CELL_PNGDEC_GRAYSCALE_ALPHA ||
        dec_info.colorSpace == CELL_PNGDEC_RGBA ||
        (dec_info.chunkInformation & 0x10)) {
        in_param.outputAlphaSelect = CELL_PNGDEC_STREAM_ALPHA;
    } else {
        in_param.outputAlphaSelect = CELL_PNGDEC_FIX_ALPHA;
    }
    in_param.outputColorAlpha = 0xff;

    CellPngDecOutParam out_param;
    memset(&out_param, 0, sizeof(out_param));
    ret = cellPngDecSetParameter(main_handle, sub_handle, &in_param, &out_param);
    if (ret != 0)
        goto cleanup_close;

    uint32_t pitch = (uint32_t)out_param.outputWidthByte;
    if (pitch == 0)
        pitch = out_param.outputWidth * 4;
    size_t alloc_size = (size_t)pitch * out_param.outputHeight;
    void *bmp = malloc(alloc_size);
    if (bmp == NULL) {
        ret = CELL_PNGDEC_ERROR_FATAL;
        goto cleanup_close;
    }
    memset(bmp, 0, alloc_size);

    CellPngDecDataCtrlParam ctrl_param;
    memset(&ctrl_param, 0, sizeof(ctrl_param));
    ctrl_param.outputBytesPerLine = pitch;

    CellPngDecDataOutInfo data_out_info;
    memset(&data_out_info, 0, sizeof(data_out_info));

    ret = cellPngDecDecodeData(main_handle, sub_handle, (uint8_t *)bmp, &ctrl_param, &data_out_info);
    if (ret == 0 && data_out_info.status == CELL_PNGDEC_DEC_STATUS_FINISH) {
        out->bmp_out = bmp;
        out->pitch = pitch;
        out->width = out_param.outputWidth;
        out->height = out_param.outputHeight;
        bmp = NULL;
    } else if (ret == 0) {
        ret = CELL_PNGDEC_ERROR_FATAL;
    }

    if (bmp != NULL)
        free(bmp);

cleanup_close:
    cellPngDecClose(main_handle, sub_handle);
    cellPngDecDestroy(main_handle);
    return ret;
}

int32_t pngLoadFromFile(const char *filename, pngData *out)
{
    if (filename == NULL || out == NULL)
        return CELL_PNGDEC_ERROR_ARG;

    CellPngDecSrc src;
    memset(&src, 0, sizeof(src));
    src.srcSelect = CELL_PNGDEC_FILE;
    src.fileName = (uint32_t)(uintptr_t)filename;
    src.spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;

    return decode_png(&src, out);
}

int32_t pngLoadFromBuffer(const void *buffer, uint32_t size, pngData *out)
{
    if (buffer == NULL || size == 0 || out == NULL)
        return CELL_PNGDEC_ERROR_ARG;

    CellPngDecSrc src;
    memset(&src, 0, sizeof(src));
    src.srcSelect = CELL_PNGDEC_BUFFER;
    src.streamPtr = (uint32_t)(uintptr_t)buffer;
    src.streamSize = size;
    src.spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;

    return decode_png(&src, out);
}
