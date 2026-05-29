/*
 * hello-ppu-jpgenc - buffer-only cellJpgEnc smoke sample.
 *
 * Generates a small RGB test image in memory, encodes it to a JPEG stream
 * buffer through libjpgenc_stub.a, and prints stream size/hash/header bytes.
 * The sample intentionally avoids filesystem input/output so it remains
 * self-contained like hello-ppu-jpg-dec.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>
#include <cell/sysmodule.h>
#include <cell/codec.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define IMAGE_WIDTH       64u
#define IMAGE_HEIGHT      64u
#define IMAGE_COMPONENTS  3u
#define OUTPUT_LIMIT      (128u * 1024u)
#define ENCODE_QUALITY    80u

static void fill_test_image(uint8_t *rgb, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *p = rgb + (y * width + x) * IMAGE_COMPONENTS;
            uint8_t checker = ((x / 8u) ^ (y / 8u)) & 1u ? 0x40u : 0xc0u;
            p[0] = (uint8_t)((x * 255u) / (width - 1u));
            p[1] = (uint8_t)((y * 255u) / (height - 1u));
            p[2] = checker;
        }
    }
}

static uint32_t fnv1a32(const uint8_t *data, uint32_t size)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static const char *jpgenc_error_name(int32_t rc)
{
    switch (rc) {
    case 0: return "CELL_OK";
    case CELL_JPGENC_ERROR_ARG: return "CELL_JPGENC_ERROR_ARG";
    case CELL_JPGENC_ERROR_SEQ: return "CELL_JPGENC_ERROR_SEQ";
    case CELL_JPGENC_ERROR_BUSY: return "CELL_JPGENC_ERROR_BUSY";
    case CELL_JPGENC_ERROR_EMPTY: return "CELL_JPGENC_ERROR_EMPTY";
    case CELL_JPGENC_ERROR_RESET: return "CELL_JPGENC_ERROR_RESET";
    case CELL_JPGENC_ERROR_FATAL: return "CELL_JPGENC_ERROR_FATAL";
    case CELL_JPGENC_ERROR_STREAM_ABORT: return "CELL_JPGENC_ERROR_STREAM_ABORT";
    case CELL_JPGENC_ERROR_STREAM_SKIP: return "CELL_JPGENC_ERROR_STREAM_SKIP";
    case CELL_JPGENC_ERROR_STREAM_OVERFLOW: return "CELL_JPGENC_ERROR_STREAM_OVERFLOW";
    case CELL_JPGENC_ERROR_STREAM_FILE_OPEN: return "CELL_JPGENC_ERROR_STREAM_FILE_OPEN";
    default: return "unknown";
    }
}

static void log_jpgenc_error(const char *where, int32_t rc)
{
    printf("  %s failed: %s (0x%08x)\n", where, jpgenc_error_name(rc), (uint32_t)rc);
}

static void print_header_bytes(const uint8_t *data, uint32_t size)
{
    uint32_t n = size < 16u ? size : 16u;
    printf("  first bytes:");
    for (uint32_t i = 0; i < n; i++)
        printf(" %02x", data[i]);
    printf("\n");
}

static int encode_generated_image(void)
{
    int32_t rc = 0;
    CellJpgEncHandle handle = 0;
    CellJpgEncConfig config;
    CellJpgEncAttr attr;
    CellJpgEncResource resource;
    CellJpgEncPicture2 picture;
    CellJpgEncEncodeParam encode_param;
    CellJpgEncOutputParam output_param;
    CellJpgEncStreamInfo stream_info;
    uint32_t stream_info_num = 0;

    uint8_t *image = NULL;
    uint8_t *stream = NULL;
    void *work_mem = NULL;

    image = malloc(IMAGE_WIDTH * IMAGE_HEIGHT * IMAGE_COMPONENTS);
    stream = malloc(OUTPUT_LIMIT);
    if (!image || !stream) {
        printf("  malloc failed\n");
        rc = -1;
        goto cleanup;
    }
    fill_test_image(image, IMAGE_WIDTH, IMAGE_HEIGHT);

    memset(&config, 0, sizeof(config));
    config.maxWidth = IMAGE_WIDTH;
    config.maxHeight = IMAGE_HEIGHT;
    config.encodeColorSpace = CELL_JPGENC_COLOR_SPACE_YCbCr;
    config.encodeSamplingFormat = CELL_JPGENC_SAMPLING_FMT_YCbCr422;
    config.enableSpu = false;

    rc = cellJpgEncQueryAttr(&config, &attr);
    if (rc < 0) {
        log_jpgenc_error("cellJpgEncQueryAttr", rc);
        goto cleanup;
    }
    printf("  attr: memSize=%u cmdQueueDepth=%u version=%08x.%08x\n",
           attr.memSize, attr.cmdQueueDepth, attr.versionUpper, attr.versionLower);

    work_mem = malloc(attr.memSize);
    if (!work_mem) {
        printf("  malloc(%u) failed\n", attr.memSize);
        rc = -1;
        goto cleanup;
    }

    memset(&resource, 0, sizeof(resource));
    resource.memAddr = work_mem;
    resource.memSize = attr.memSize;
    resource.ppuThreadPriority = 1000;
    resource.spuThreadPriority = 200;

    rc = cellJpgEncOpen(&config, &resource, &handle);
    if (rc < 0) {
        log_jpgenc_error("cellJpgEncOpen", rc);
        goto cleanup;
    }

    rc = cellJpgEncWaitForInput(handle, true);
    if (rc < 0) {
        log_jpgenc_error("cellJpgEncWaitForInput", rc);
        goto close_encoder;
    }

    memset(&picture, 0, sizeof(picture));
    picture.width = IMAGE_WIDTH;
    picture.height = IMAGE_HEIGHT;
    picture.pitchWidth = IMAGE_WIDTH * IMAGE_COMPONENTS;
    picture.colorSpace = CELL_JPGENC_COLOR_SPACE_RGB;
    picture.pictureAddr = image;

    memset(&encode_param, 0, sizeof(encode_param));
    encode_param.enableSpu = false;
    encode_param.restartInterval = 0;
    encode_param.dctMethod = CELL_JPGENC_DCT_METHOD_FAST;
    encode_param.compressionMode = CELL_JPGENC_COMPR_MODE_STREAM_SIZE_LIMIT;
    encode_param.quality = ENCODE_QUALITY;

    memset(&output_param, 0, sizeof(output_param));
    output_param.location = CELL_JPGENC_LOCATION_BUFFER;
    output_param.streamAddr = stream;
    output_param.limitSize = OUTPUT_LIMIT;

    printf("  input: %ux%u RGB checker-gradient, quality=%u\n",
           IMAGE_WIDTH, IMAGE_HEIGHT, ENCODE_QUALITY);
    rc = cellJpgEncEncodePicture2(handle, &picture, &encode_param, &output_param);
    if (rc < 0) {
        log_jpgenc_error("cellJpgEncEncodePicture2", rc);
        goto close_encoder;
    }

    rc = cellJpgEncWaitForOutput(handle, &stream_info_num, true);
    if (rc < 0) {
        log_jpgenc_error("cellJpgEncWaitForOutput", rc);
        goto close_encoder;
    }
    printf("  streamInfoNum=%u\n", stream_info_num);

    memset(&stream_info, 0, sizeof(stream_info));
    rc = cellJpgEncGetStreamInfo(handle, &stream_info);
    if (rc < 0) {
        log_jpgenc_error("cellJpgEncGetStreamInfo", rc);
        goto close_encoder;
    }
    if (stream_info.state < 0) {
        log_jpgenc_error("streamInfo.state", stream_info.state);
        rc = stream_info.state;
        goto close_encoder;
    }
    if (!stream_info.streamAddr || stream_info.streamSize == 0) {
        printf("  empty encoder output\n");
        rc = -1;
        goto close_encoder;
    }

    const uint8_t *encoded = (const uint8_t *)stream_info.streamAddr;
    printf("  output: size=%u quality=%u hash=0x%08x\n",
           stream_info.streamSize, stream_info.quality,
           fnv1a32(encoded, stream_info.streamSize));
    print_header_bytes(encoded, stream_info.streamSize);

close_encoder:
    {
        int32_t close_rc = cellJpgEncClose(handle);
        if (rc == 0 && close_rc < 0) {
            log_jpgenc_error("cellJpgEncClose", close_rc);
            rc = close_rc;
        }
    }
cleanup:
    free(work_mem);
    free(stream);
    free(image);
    return rc < 0 ? 1 : 0;
}

int main(void)
{
    printf("hello-ppu-jpgenc: buffer-only JPEG encode\n");

    int32_t rc = cellSysmoduleLoadModule(CELL_SYSMODULE_JPGENC);
    if (rc < 0) {
        log_jpgenc_error("CELL_SYSMODULE_JPGENC load", rc);
        return 1;
    }

    int result = encode_generated_image();

    rc = cellSysmoduleUnloadModule(CELL_SYSMODULE_JPGENC);
    if (rc < 0) {
        log_jpgenc_error("CELL_SYSMODULE_JPGENC unload", rc);
        return 1;
    }

    printf("hello-ppu-jpgenc: %s\n", result == 0 ? "done" : "failed");
    return result;
}
