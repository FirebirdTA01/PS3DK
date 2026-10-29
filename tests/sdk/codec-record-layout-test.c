/* Compile-time ABI guard for <cell/codec/gifdec.h>, <cell/codec/jpgenc.h>,
 * <cell/codec/pngenc.h>, <cell/codec/jpgdec.h>, <cell/codec/pngdec.h>: the system's multimedia services exchange these
 * records with user space, so every size, alignment and offset is part of
 * the system interface. Both data models use the system's 32-bit layout;
 * pointer members and callback slots are 32-bit effective addresses
 * (ATTRIBUTE_PRXPTR). */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <cell/codec/gifdec.h>
#include <cell/codec/jpgenc.h>
#include <cell/codec/pngenc.h>
#include <cell/codec/jpgdec.h>
#include <cell/codec/pngdec.h>

#ifdef __cplusplus
#define CHECK(c, m) static_assert(c, m)
#else
#define CHECK(c, m) _Static_assert(c, m)
#endif
#define SIZE(t, n) CHECK(sizeof(t) == (n), #t " size")
#define ALIGN(t, n) CHECK(__alignof__(t) == (n), #t " alignment")
#define OFFSET(t, f, n) CHECK(offsetof(t, f) == (n), #t "." #f " offset")
#define WIDTH(t, f, n) CHECK(sizeof(((t *)0)->f) == (n), #t "." #f " width")

/* The runner passes the data model it asked for; prove the compiler used it. */
CHECK(sizeof(void *) == EXPECTED_ABI_BYTES, "actual pointer data model");
CHECK(sizeof(long) == EXPECTED_ABI_BYTES, "actual long data model");

/* Mutant controls for test runner verification */
#if defined(MUTANT_GIFDEC_THREAD_IN_PARAM_SIZE)
CHECK(sizeof(CellGifDecThreadInParam) == 40, "mutant: old 40-byte thread in param");
#elif defined(MUTANT_GIFDEC_SRC_SIZE)
CHECK(sizeof(CellGifDecSrc) == 40, "mutant: old 40-byte src size");
#elif defined(MUTANT_JPGENC_OUTPUT_PARAM_SIZE)
CHECK(sizeof(CellJpgEncOutputParam) == 24, "mutant: old 24-byte jpgenc output param");
#elif defined(MUTANT_JPGENC_STREAM_INFO_STREAMADDR)
CHECK(offsetof(CellJpgEncStreamInfo, streamAddr) == 16, "mutant: old jpgenc streamAddr offset");
#elif defined(MUTANT_PNGENC_OUTPUT_PARAM_SIZE)
CHECK(sizeof(CellPngEncOutputParam) == 24, "mutant: old 24-byte pngenc output param");
#elif defined(MUTANT_PNGENC_STREAM_INFO_STREAMADDR)
CHECK(offsetof(CellPngEncStreamInfo, streamAddr) == 16, "mutant: old pngenc streamAddr offset");
#endif

/* 1. CellGifDecThreadInParam (gifdec.h) */
SIZE(CellGifDecThreadInParam, 28);
ALIGN(CellGifDecThreadInParam, 4);
OFFSET(CellGifDecThreadInParam, spuThreadEnable, 0);
WIDTH(CellGifDecThreadInParam, spuThreadEnable, 4);
OFFSET(CellGifDecThreadInParam, ppuThreadPriority, 4);
WIDTH(CellGifDecThreadInParam, ppuThreadPriority, 4);
OFFSET(CellGifDecThreadInParam, spuThreadPriority, 8);
WIDTH(CellGifDecThreadInParam, spuThreadPriority, 4);
OFFSET(CellGifDecThreadInParam, cbCtrlMallocFunc, 12);
WIDTH(CellGifDecThreadInParam, cbCtrlMallocFunc, 4);
OFFSET(CellGifDecThreadInParam, cbCtrlMallocArg, 16);
WIDTH(CellGifDecThreadInParam, cbCtrlMallocArg, 4);
OFFSET(CellGifDecThreadInParam, cbCtrlFreeFunc, 20);
WIDTH(CellGifDecThreadInParam, cbCtrlFreeFunc, 4);
OFFSET(CellGifDecThreadInParam, cbCtrlFreeArg, 24);
WIDTH(CellGifDecThreadInParam, cbCtrlFreeArg, 4);

/* 2. CellGifDecSrc (gifdec.h) */
SIZE(CellGifDecSrc, 32);
ALIGN(CellGifDecSrc, 8);
OFFSET(CellGifDecSrc, srcSelect, 0);
WIDTH(CellGifDecSrc, srcSelect, 4);
OFFSET(CellGifDecSrc, fileName, 4);
WIDTH(CellGifDecSrc, fileName, 4);
OFFSET(CellGifDecSrc, fileOffset, 8);
WIDTH(CellGifDecSrc, fileOffset, 8);
OFFSET(CellGifDecSrc, fileSize, 16);
WIDTH(CellGifDecSrc, fileSize, 4);
OFFSET(CellGifDecSrc, streamPtr, 20);
WIDTH(CellGifDecSrc, streamPtr, 4);
OFFSET(CellGifDecSrc, streamSize, 24);
WIDTH(CellGifDecSrc, streamSize, 4);
OFFSET(CellGifDecSrc, spuThreadEnable, 28);
WIDTH(CellGifDecSrc, spuThreadEnable, 4);

/* 3. CellJpgEncOutputParam (jpgenc.h) */
SIZE(CellJpgEncOutputParam, 16);
ALIGN(CellJpgEncOutputParam, 4);
OFFSET(CellJpgEncOutputParam, location, 0);
WIDTH(CellJpgEncOutputParam, location, 4);
OFFSET(CellJpgEncOutputParam, streamFileName, 4);
WIDTH(CellJpgEncOutputParam, streamFileName, 4);
OFFSET(CellJpgEncOutputParam, streamAddr, 8);
WIDTH(CellJpgEncOutputParam, streamAddr, 4);
OFFSET(CellJpgEncOutputParam, limitSize, 12);
WIDTH(CellJpgEncOutputParam, limitSize, 4);

/* 4. CellJpgEncStreamInfo (jpgenc.h) */
SIZE(CellJpgEncStreamInfo, 40);
ALIGN(CellJpgEncStreamInfo, 8);
OFFSET(CellJpgEncStreamInfo, state, 0);
WIDTH(CellJpgEncStreamInfo, state, 4);
OFFSET(CellJpgEncStreamInfo, location, 4);
WIDTH(CellJpgEncStreamInfo, location, 4);
OFFSET(CellJpgEncStreamInfo, streamFileName, 8);
WIDTH(CellJpgEncStreamInfo, streamFileName, 4);
OFFSET(CellJpgEncStreamInfo, streamAddr, 12);
WIDTH(CellJpgEncStreamInfo, streamAddr, 4);
OFFSET(CellJpgEncStreamInfo, limitSize, 16);
WIDTH(CellJpgEncStreamInfo, limitSize, 4);
OFFSET(CellJpgEncStreamInfo, streamSize, 20);
WIDTH(CellJpgEncStreamInfo, streamSize, 4);
OFFSET(CellJpgEncStreamInfo, processedLine, 24);
WIDTH(CellJpgEncStreamInfo, processedLine, 4);
OFFSET(CellJpgEncStreamInfo, quality, 28);
WIDTH(CellJpgEncStreamInfo, quality, 4);
OFFSET(CellJpgEncStreamInfo, userData, 32);
WIDTH(CellJpgEncStreamInfo, userData, 8);

/* 5. CellPngEncOutputParam (pngenc.h) */
SIZE(CellPngEncOutputParam, 16);
ALIGN(CellPngEncOutputParam, 4);
OFFSET(CellPngEncOutputParam, location, 0);
WIDTH(CellPngEncOutputParam, location, 4);
OFFSET(CellPngEncOutputParam, streamFileName, 4);
WIDTH(CellPngEncOutputParam, streamFileName, 4);
OFFSET(CellPngEncOutputParam, streamAddr, 8);
WIDTH(CellPngEncOutputParam, streamAddr, 4);
OFFSET(CellPngEncOutputParam, limitSize, 12);
WIDTH(CellPngEncOutputParam, limitSize, 4);

/* 6. CellPngEncStreamInfo (pngenc.h) */
SIZE(CellPngEncStreamInfo, 40);
ALIGN(CellPngEncStreamInfo, 8);
OFFSET(CellPngEncStreamInfo, state, 0);
WIDTH(CellPngEncStreamInfo, state, 4);
OFFSET(CellPngEncStreamInfo, location, 4);
WIDTH(CellPngEncStreamInfo, location, 4);
OFFSET(CellPngEncStreamInfo, streamFileName, 8);
WIDTH(CellPngEncStreamInfo, streamFileName, 4);
OFFSET(CellPngEncStreamInfo, streamAddr, 12);
WIDTH(CellPngEncStreamInfo, streamAddr, 4);
OFFSET(CellPngEncStreamInfo, limitSize, 16);
WIDTH(CellPngEncStreamInfo, limitSize, 4);
OFFSET(CellPngEncStreamInfo, streamSize, 20);
WIDTH(CellPngEncStreamInfo, streamSize, 4);
OFFSET(CellPngEncStreamInfo, processedLine, 24);
WIDTH(CellPngEncStreamInfo, processedLine, 4);
OFFSET(CellPngEncStreamInfo, userData, 32);
WIDTH(CellPngEncStreamInfo, userData, 8);

/* 7. CellJpgDecStrmParam (jpgdec.h) */
SIZE(CellJpgDecStrmParam, 8);
ALIGN(CellJpgDecStrmParam, 4);
OFFSET(CellJpgDecStrmParam, strmPtr, 0);
WIDTH(CellJpgDecStrmParam, strmPtr, 4);
OFFSET(CellJpgDecStrmParam, strmSize, 4);
WIDTH(CellJpgDecStrmParam, strmSize, 4);

/* 8. CellPngDecStrmParam (pngdec.h) */
SIZE(CellPngDecStrmParam, 8);
ALIGN(CellPngDecStrmParam, 4);
OFFSET(CellPngDecStrmParam, strmPtr, 0);
WIDTH(CellPngDecStrmParam, strmPtr, 4);
OFFSET(CellPngDecStrmParam, strmSize, 4);
WIDTH(CellPngDecStrmParam, strmSize, 4);

/* 9. CellJpgDecDispInfo (jpgdec.h) */
SIZE(CellJpgDecDispInfo, 56);
ALIGN(CellJpgDecDispInfo, 8);
OFFSET(CellJpgDecDispInfo, outputFrameWidthByte, 0);
WIDTH(CellJpgDecDispInfo, outputFrameWidthByte, 8);
OFFSET(CellJpgDecDispInfo, outputFrameHeight, 8);
WIDTH(CellJpgDecDispInfo, outputFrameHeight, 4);
OFFSET(CellJpgDecDispInfo, outputStartXByte, 16);
WIDTH(CellJpgDecDispInfo, outputStartXByte, 8);
OFFSET(CellJpgDecDispInfo, outputStartY, 24);
WIDTH(CellJpgDecDispInfo, outputStartY, 4);
OFFSET(CellJpgDecDispInfo, outputWidthByte, 32);
WIDTH(CellJpgDecDispInfo, outputWidthByte, 8);
OFFSET(CellJpgDecDispInfo, outputHeight, 40);
WIDTH(CellJpgDecDispInfo, outputHeight, 4);
OFFSET(CellJpgDecDispInfo, outputComponents, 44);
WIDTH(CellJpgDecDispInfo, outputComponents, 4);
OFFSET(CellJpgDecDispInfo, outputImage, 48);
WIDTH(CellJpgDecDispInfo, outputImage, 4);

/* 10. CellPngDecDispInfo (pngdec.h) */
SIZE(CellPngDecDispInfo, 64);
ALIGN(CellPngDecDispInfo, 8);
OFFSET(CellPngDecDispInfo, outputFrameWidthByte, 0);
WIDTH(CellPngDecDispInfo, outputFrameWidthByte, 8);
OFFSET(CellPngDecDispInfo, outputFrameHeight, 8);
WIDTH(CellPngDecDispInfo, outputFrameHeight, 4);
OFFSET(CellPngDecDispInfo, outputStartXByte, 16);
WIDTH(CellPngDecDispInfo, outputStartXByte, 8);
OFFSET(CellPngDecDispInfo, outputStartY, 24);
WIDTH(CellPngDecDispInfo, outputStartY, 4);
OFFSET(CellPngDecDispInfo, outputWidthByte, 32);
WIDTH(CellPngDecDispInfo, outputWidthByte, 8);
OFFSET(CellPngDecDispInfo, outputHeight, 40);
WIDTH(CellPngDecDispInfo, outputHeight, 4);
OFFSET(CellPngDecDispInfo, outputBitDepth, 44);
WIDTH(CellPngDecDispInfo, outputBitDepth, 4);
OFFSET(CellPngDecDispInfo, outputComponents, 48);
WIDTH(CellPngDecDispInfo, outputComponents, 4);
OFFSET(CellPngDecDispInfo, nextOutputStartY, 52);
WIDTH(CellPngDecDispInfo, nextOutputStartY, 4);
OFFSET(CellPngDecDispInfo, scanPassCount, 56);
WIDTH(CellPngDecDispInfo, scanPassCount, 4);
OFFSET(CellPngDecDispInfo, outputImage, 60);
WIDTH(CellPngDecDispInfo, outputImage, 4);

/* Compile-time verification that attributed pointers remain pointer types,
 * not integers, so callers retain normal pointer syntax and assignments. */
void codec_pointer_type_semantics_check(void);
void codec_pointer_type_semantics_check(void) {
    CellGifDecThreadInParam gtip;
    CellGifDecSrc gsrc;
    CellJpgEncOutputParam jop;
    CellJpgEncStreamInfo jsi;
    CellPngEncOutputParam pop;
    CellPngEncStreamInfo psi;

    void *vp = (void *)0;
    const char *cp = (const char *)0;

    gtip.cbCtrlMallocArg = vp;
    gtip.cbCtrlFreeArg = vp;
    gsrc.fileName = cp;
    jop.streamFileName = cp;
    jsi.streamFileName = cp;
    pop.streamFileName = cp;
    psi.streamFileName = cp;

    (void)gtip;
    (void)gsrc;
    (void)jop;
    (void)jsi;
    (void)pop;
    (void)psi;
}

/* Judged byte-buffer witness: at link time under -O2, constant-evaluable
 * assertions fold away entirely; any mismatch leaves an unresolved reference to
 * codec_witness_decode_mismatch(). */
#ifdef __cplusplus
extern "C" {
#endif
extern void codec_witness_decode_mismatch(void);
#ifdef __cplusplus
}
#endif

#define CODEC_WITNESS_CHECK(cond) do { \
    if (!(cond)) { \
        codec_witness_decode_mismatch(); \
    } \
} while (0)

void witness_verify_codec_boundaries(void);
void witness_verify_codec_boundaries(void) {
    /* 1. CellGifDecThreadInParam:
     * [0..3] spuThreadEnable: 1, [4..7] ppuThreadPriority: 100, [8..11] spuThreadPriority: 200,
     * [12..15] cbCtrlMallocFunc: 0x10000004, [16..19] cbCtrlMallocArg: 0x10000008,
     * [20..23] cbCtrlFreeFunc: 0x1000000c, [24..27] cbCtrlFreeArg: 0x10000010 */
    unsigned char gtip_buf[sizeof(CellGifDecThreadInParam)];
    memset(gtip_buf, 0, sizeof(gtip_buf));
    gtip_buf[3] = 1;
    gtip_buf[7] = 100;
    gtip_buf[11] = 200;
    gtip_buf[12] = 0x10; gtip_buf[15] = 0x04;
    gtip_buf[16] = 0x10; gtip_buf[19] = 0x08;
    gtip_buf[20] = 0x10; gtip_buf[23] = 0x0c;
    gtip_buf[24] = 0x10; gtip_buf[27] = 0x10;

    CellGifDecThreadInParam gtip;
    memcpy(&gtip, gtip_buf, sizeof(gtip));
    CODEC_WITNESS_CHECK(gtip.spuThreadEnable == 1);
    CODEC_WITNESS_CHECK(gtip.ppuThreadPriority == 100);
    CODEC_WITNESS_CHECK(gtip.spuThreadPriority == 200);
    CODEC_WITNESS_CHECK((uint32_t)gtip.cbCtrlMallocFunc == 0x10000004U);
    CODEC_WITNESS_CHECK((uint32_t)gtip.cbCtrlMallocArg == 0x10000008U);
    CODEC_WITNESS_CHECK((uint32_t)gtip.cbCtrlFreeFunc == 0x1000000cU);
    CODEC_WITNESS_CHECK((uint32_t)gtip.cbCtrlFreeArg == 0x10000010U);

    /* 2. CellGifDecSrc:
     * [0..3] srcSelect: 1, [4..7] fileName: 0x20000004, [8..15] fileOffset: 0x1000000020000000ULL,
     * [16..19] fileSize: 0x00010000, [20..23] streamPtr: 0x20000014, [24..27] streamSize: 0x00020000,
     * [28..31] spuThreadEnable: 1 */
    unsigned char gsrc_buf[sizeof(CellGifDecSrc)];
    memset(gsrc_buf, 0, sizeof(gsrc_buf));
    gsrc_buf[3] = 1;
    gsrc_buf[4] = 0x20; gsrc_buf[7] = 0x04;
    gsrc_buf[8] = 0x10; gsrc_buf[12] = 0x20;
    gsrc_buf[17] = 0x01;
    gsrc_buf[20] = 0x20; gsrc_buf[23] = 0x14;
    gsrc_buf[25] = 0x02;
    gsrc_buf[31] = 1;

    CellGifDecSrc gsrc;
    memcpy(&gsrc, gsrc_buf, sizeof(gsrc));
    CODEC_WITNESS_CHECK(gsrc.srcSelect == 1);
    CODEC_WITNESS_CHECK((uint32_t)gsrc.fileName == 0x20000004U);
    CODEC_WITNESS_CHECK(gsrc.fileOffset == 0x1000000020000000ULL);
    CODEC_WITNESS_CHECK(gsrc.fileSize == 0x00010000U);
    CODEC_WITNESS_CHECK((uint32_t)gsrc.streamPtr == 0x20000014U);
    CODEC_WITNESS_CHECK(gsrc.streamSize == 0x00020000U);
    CODEC_WITNESS_CHECK(gsrc.spuThreadEnable == 1);

    /* 3. CellJpgEncOutputParam:
     * [0..3] location: 1, [4..7] streamFileName: 0x30000004, [8..11] streamAddr: 0x30000008, [12..15] limitSize: 0x00040000 */
    unsigned char jop_buf[sizeof(CellJpgEncOutputParam)];
    memset(jop_buf, 0, sizeof(jop_buf));
    jop_buf[3] = 1;
    jop_buf[4] = 0x30; jop_buf[7] = 0x04;
    jop_buf[8] = 0x30; jop_buf[11] = 0x08;
    jop_buf[13] = 0x04;

    CellJpgEncOutputParam jop;
    memcpy(&jop, jop_buf, sizeof(jop));
    CODEC_WITNESS_CHECK(jop.location == 1);
    CODEC_WITNESS_CHECK((uint32_t)jop.streamFileName == 0x30000004U);
    CODEC_WITNESS_CHECK((uint32_t)jop.streamAddr == 0x30000008U);
    CODEC_WITNESS_CHECK(jop.limitSize == 0x00040000U);

    /* 4. CellJpgEncStreamInfo:
     * [0..3] state: 2, [4..7] location: 1, [8..11] streamFileName: 0x40000008, [12..15] streamAddr: 0x4000000c,
     * [16..19] limitSize: 0x00050000, [20..23] streamSize: 0x00028000, [24..27] processedLine: 1080,
     * [28..31] quality: 95, [32..39] userData: 0x5000000060000000ULL */
    unsigned char jsi_buf[sizeof(CellJpgEncStreamInfo)];
    memset(jsi_buf, 0, sizeof(jsi_buf));
    jsi_buf[3] = 2;
    jsi_buf[7] = 1;
    jsi_buf[8] = 0x40; jsi_buf[11] = 0x08;
    jsi_buf[12] = 0x40; jsi_buf[15] = 0x0c;
    jsi_buf[17] = 0x05;
    jsi_buf[21] = 0x02; jsi_buf[22] = 0x80;
    jsi_buf[26] = 0x04; jsi_buf[27] = 0x38;
    jsi_buf[31] = 95;
    jsi_buf[32] = 0x50; jsi_buf[36] = 0x60;

    CellJpgEncStreamInfo jsi;
    memcpy(&jsi, jsi_buf, sizeof(jsi));
    CODEC_WITNESS_CHECK(jsi.state == 2);
    CODEC_WITNESS_CHECK(jsi.location == 1);
    CODEC_WITNESS_CHECK((uint32_t)jsi.streamFileName == 0x40000008U);
    CODEC_WITNESS_CHECK((uint32_t)jsi.streamAddr == 0x4000000cU);
    CODEC_WITNESS_CHECK(jsi.limitSize == 0x00050000U);
    CODEC_WITNESS_CHECK(jsi.streamSize == 0x00028000U);
    CODEC_WITNESS_CHECK(jsi.processedLine == 1080);
    CODEC_WITNESS_CHECK(jsi.quality == 95);
    CODEC_WITNESS_CHECK(jsi.userData == 0x5000000060000000ULL);

    /* 5. CellPngEncOutputParam:
     * [0..3] location: 1, [4..7] streamFileName: 0x60000004, [8..11] streamAddr: 0x60000008, [12..15] limitSize: 0x00080000 */
    unsigned char pop_buf[sizeof(CellPngEncOutputParam)];
    memset(pop_buf, 0, sizeof(pop_buf));
    pop_buf[3] = 1;
    pop_buf[4] = 0x60; pop_buf[7] = 0x04;
    pop_buf[8] = 0x60; pop_buf[11] = 0x08;
    pop_buf[13] = 0x08;

    CellPngEncOutputParam pop;
    memcpy(&pop, pop_buf, sizeof(pop));
    CODEC_WITNESS_CHECK(pop.location == 1);
    CODEC_WITNESS_CHECK((uint32_t)pop.streamFileName == 0x60000004U);
    CODEC_WITNESS_CHECK((uint32_t)pop.streamAddr == 0x60000008U);
    CODEC_WITNESS_CHECK(pop.limitSize == 0x00080000U);

    /* 6. CellPngEncStreamInfo:
     * [0..3] state: 1, [4..7] location: 1, [8..11] streamFileName: 0x70000008, [12..15] streamAddr: 0x7000000c,
     * [16..19] limitSize: 0x00080000, [20..23] streamSize: 0x00040000, [24..27] processedLine: 720,
     * [32..39] userData: 0x7000000080000000ULL */
    unsigned char psi_buf[sizeof(CellPngEncStreamInfo)];
    memset(psi_buf, 0, sizeof(psi_buf));
    psi_buf[3] = 1;
    psi_buf[7] = 1;
    psi_buf[8] = 0x70; psi_buf[11] = 0x08;
    psi_buf[12] = 0x70; psi_buf[15] = 0x0c;
    psi_buf[17] = 0x08;
    psi_buf[21] = 0x04;
    psi_buf[26] = 0x02; psi_buf[27] = 0xd0;
    psi_buf[32] = 0x70; psi_buf[36] = 0x80;

    CellPngEncStreamInfo psi;
    memcpy(&psi, psi_buf, sizeof(psi));
    CODEC_WITNESS_CHECK(psi.state == 1);
    CODEC_WITNESS_CHECK(psi.location == 1);
    CODEC_WITNESS_CHECK((uint32_t)psi.streamFileName == 0x70000008U);
    CODEC_WITNESS_CHECK((uint32_t)psi.streamAddr == 0x7000000cU);
    CODEC_WITNESS_CHECK(psi.limitSize == 0x00080000U);
    CODEC_WITNESS_CHECK(psi.streamSize == 0x00040000U);
    CODEC_WITNESS_CHECK(psi.processedLine == 720);
    CODEC_WITNESS_CHECK(psi.userData == 0x7000000080000000ULL);
}

int main(void) {
    codec_pointer_type_semantics_check();
    witness_verify_codec_boundaries();
    return 0;
}
