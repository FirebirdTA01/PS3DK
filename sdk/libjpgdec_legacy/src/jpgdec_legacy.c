/* sdk/libjpgdec_legacy/src/jpgdec_legacy.c
 *
 * PSL1GHT libjpgdec legacy convenience API: jpgLoadFromFile and jpgLoadFromBuffer.
 *
 * All other PSL1GHT jpgDec* names are nidgen aliases of the reference cellJpgDec*
 * exports (libjpgdec_stub.yaml).
 *
 * This implementation wraps the reference cellJpgDec API cleanly without
 * depending on PSL1GHT's original implementation.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cell/codec/jpgdec.h>

typedef struct _jpg_data {
    void *bmp_out;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
} jpgData;

int32_t jpgLoadFromFile(const char *filename, jpgData *out);
int32_t jpgLoadFromBuffer(const void *buffer, uint32_t size, jpgData *out);

static void *jpg_cb_malloc(uint32_t size, void *arg)
{
    (void)arg;
    return malloc(size);
}

static int32_t jpg_cb_free(void *ptr, void *arg)
{
    (void)arg;
    free(ptr);
    return 0;
}

static int32_t decode_jpeg(const CellJpgDecSrc *src, jpgData *out)
{
    if (src == NULL || out == NULL)
        return CELL_JPGDEC_ERROR_ARG;

    out->bmp_out = NULL;
    out->pitch = 0;
    out->width = 0;
    out->height = 0;

    CellJpgDecThreadInParam thread_in;
    memset(&thread_in, 0, sizeof(thread_in));
    thread_in.spuThreadEnable = CELL_JPGDEC_SPU_THREAD_DISABLE;
    thread_in.ppuThreadPriority = 512;
    thread_in.spuThreadPriority = 200;
    thread_in.cbCtrlMallocFunc = jpg_cb_malloc;
    thread_in.cbCtrlFreeFunc = jpg_cb_free;

    CellJpgDecThreadOutParam thread_out;
    memset(&thread_out, 0, sizeof(thread_out));

    CellJpgDecMainHandle main_handle = 0;
    int32_t ret = cellJpgDecCreate(&main_handle, &thread_in, &thread_out);
    if (ret != 0)
        return ret;

    CellJpgDecSubHandle sub_handle = 0;
    CellJpgDecOpnInfo open_info;
    memset(&open_info, 0, sizeof(open_info));

    ret = cellJpgDecOpen(main_handle, &sub_handle, src, &open_info);
    if (ret != 0) {
        cellJpgDecDestroy(main_handle);
        return ret;
    }

    CellJpgDecInfo dec_info;
    memset(&dec_info, 0, sizeof(dec_info));
    ret = cellJpgDecReadHeader(main_handle, sub_handle, &dec_info);
    if (ret != 0 || dec_info.jpegColorSpace == CELL_JPG_UNKNOWN) {
        if (ret == 0)
            ret = CELL_JPGDEC_ERROR_STREAM_FORMAT;
        goto cleanup_close;
    }

    CellJpgDecInParam in_param;
    memset(&in_param, 0, sizeof(in_param));
    in_param.downScale = 1;
    in_param.method = CELL_JPGDEC_FAST;
    in_param.outputMode = CELL_JPGDEC_TOP_TO_BOTTOM;
    in_param.outputColorSpace = CELL_JPG_ARGB;
    in_param.outputColorAlpha = 0xff;

    CellJpgDecOutParam out_param;
    memset(&out_param, 0, sizeof(out_param));
    ret = cellJpgDecSetParameter(main_handle, sub_handle, &in_param, &out_param);
    if (ret != 0)
        goto cleanup_close;

    uint32_t pitch = (uint32_t)out_param.outputWidthByte;
    if (pitch == 0)
        pitch = out_param.outputWidth * 4;
    size_t alloc_size = (size_t)pitch * out_param.outputHeight;
    void *bmp = malloc(alloc_size);
    if (bmp == NULL) {
        ret = CELL_JPGDEC_ERROR_FATAL;
        goto cleanup_close;
    }
    memset(bmp, 0, alloc_size);

    CellJpgDecDataCtrlParam ctrl_param;
    memset(&ctrl_param, 0, sizeof(ctrl_param));
    ctrl_param.outputBytesPerLine = pitch;

    CellJpgDecDataOutInfo data_out_info;
    memset(&data_out_info, 0, sizeof(data_out_info));

    ret = cellJpgDecDecodeData(main_handle, sub_handle, (uint8_t *)bmp, &ctrl_param, &data_out_info);
    if (ret == 0 && data_out_info.status == CELL_JPGDEC_DEC_STATUS_FINISH) {
        out->bmp_out = bmp;
        out->pitch = pitch;
        out->width = out_param.outputWidth;
        out->height = out_param.outputHeight;
        bmp = NULL;
    } else if (ret == 0) {
        ret = CELL_JPGDEC_ERROR_FATAL;
    }

    if (bmp != NULL)
        free(bmp);

cleanup_close:
    cellJpgDecClose(main_handle, sub_handle);
    cellJpgDecDestroy(main_handle);
    return ret;
}

int32_t jpgLoadFromFile(const char *filename, jpgData *out)
{
    if (filename == NULL || out == NULL)
        return CELL_JPGDEC_ERROR_ARG;

    CellJpgDecSrc src;
    memset(&src, 0, sizeof(src));
    src.srcSelect = CELL_JPGDEC_FILE;
    src.fileName = filename;
    src.spuThreadEnable = CELL_JPGDEC_SPU_THREAD_DISABLE;

    return decode_jpeg(&src, out);
}

int32_t jpgLoadFromBuffer(const void *buffer, uint32_t size, jpgData *out)
{
    if (buffer == NULL || size == 0 || out == NULL)
        return CELL_JPGDEC_ERROR_ARG;

    CellJpgDecSrc src;
    memset(&src, 0, sizeof(src));
    src.srcSelect = CELL_JPGDEC_BUFFER;
    src.streamPtr = (void *)buffer;
    src.streamSize = size;
    src.spuThreadEnable = CELL_JPGDEC_SPU_THREAD_DISABLE;

    return decode_jpeg(&src, out);
}
