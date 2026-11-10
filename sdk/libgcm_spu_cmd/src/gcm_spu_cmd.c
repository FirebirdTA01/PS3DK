/*
 * PS3 Custom Toolchain — sdk/libgcm_spu_cmd/src/gcm_spu_cmd.c
 *
 * SPU GCM command writer (<cell/gcm/gcm_spu_cmd.h>).  Portable C: the SPU
 * library and the host encoding test build this same file.
 *
 * Method words: bits 18-28 are the argument count, bits 13-15 the
 * subchannel, bits 2-12 the method address; bit 30 makes the method
 * non-incrementing.  JUMP is 0x20000000 | offset, CALL offset | 2,
 * RETURN 0x00020000.  The method addresses below are the NV40 3D-class
 * (and, for labels, channel) registers named in sdk/include/rsx/nv40.h.
 *
 * Every call computes its full size first and reserves it in one go, so a
 * command is written whole or refused whole, and a measuring context
 * counts exactly what a writing context would write.
 */
#include <stdint.h>
#include <string.h>
#include <cell/gcm/gcm_spu_cmd.h>

#define FN CELL_GCM_SPU_FN

#define M(method, count)      (((uint32_t)(count) << 18) | (uint32_t)(method))
#define MNI(method, count)    (M(method, count) | 0x40000000u)
#define MSUB(ch, method, n)   (((uint32_t)(n) << 18) | ((uint32_t)(ch) << 13) | (uint32_t)(method))

enum {
    JUMP_FLAG = 0x20000000u, CALL_FLAG = 0x00000002u, RETURN_WORD = 0x00020000u,
    MAX_COUNT = 0x7ffu,

    /* channel */
    CH_SEMAPHORE_OFFSET = 0x0064, CH_SEMAPHORE_ACQUIRE = 0x0068, CH_SEMAPHORE_RELEASE = 0x006c,
    /* 3D class */
    NOP = 0x0100, WAIT_FOR_IDLE = 0x0110,
    BLEND_ENABLE = 0x0310, BLEND_FUNC_SRC = 0x0314, BLEND_EQUATION = 0x0320, COLOR_MASK = 0x0324,
    MRT_COLOR_MASK = 0x0370, DEPTH_RANGE = 0x0394,
    SCISSOR_HORIZ = 0x08c0, FP_ADDRESS = 0x08e4,
    VIEWPORT_HORIZ = 0x0a00, VIEWPORT_OFFSET = 0x0a20,
    POLYGON_OFFSET_FILL_ENABLE = 0x0a68, DEPTH_FUNC = 0x0a6c, DEPTH_WRITE_ENABLE = 0x0a70,
    DEPTH_TEST_ENABLE = 0x0a74, POLYGON_OFFSET_FACTOR = 0x0a78,
    VTXBUF_ADDRESS0 = 0x1680, VTX_CACHE_INVALIDATE2 = 0x1710, VTX_CACHE_INVALIDATE = 0x1714,
    VTXFMT0 = 0x1740, BEGIN_END = 0x1808, VB_VERTEX_BATCH = 0x1814, VB_INDEX_BATCH_OFFSET = 0x181c,
    VB_INDEX_BATCH_DRAW = 0x1824, FRONT_FACE = 0x1834, CULL_FACE_ENABLE = 0x183c,
    TEX_SIZE1_0 = 0x1840,
    TEX_OFFSET0 = 0x1a00, TEX_WRAP0 = 0x1a08, TEX_ENABLE0 = 0x1a0c, TEX_SWIZZLE0 = 0x1a10,
    TEX_FILTER0 = 0x1a14, TEX_SIZE0_0 = 0x1a18,
    VTX_ATTR_4F_X0 = 0x1c00,
    SEMAPHORE_OFFSET = 0x1d6c, SEMAPHORE_BACKEND_RELEASE = 0x1d70,
    CLEAR_VALUE_COLOR = 0x1d90, CLEAR_BUFFERS = 0x1d94,
    VP_UPLOAD_CONST_ID = 0x1efc, DIVIDER_FREQUENCY_OP = 0x1fc0, TEX_CACHE_CTL = 0x1fd8,
    /* 2D surface (subchannel 3) and image-from-CPU (subchannel 5) */
    SURF2D_DMA_IMAGE_DESTIN = 0x0188, SURF2D_FORMAT = 0x0300, SURF2D_OFFSET_DESTIN = 0x030c,
    SURF2D_FORMAT_Y32 = 0x0b, IFC_POINT = 0x0304, IFC_COLOR0 = 0x0400,
    DMA_MEMORY_FRAME_BUFFER = 0xfeed0000u,
    INDEX_TYPE_32 = 0
};

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ---- context ---------------------------------------------------------- */

void FN(SpuSetupContextData)(CellGcmSpuContextData *c, uint32_t *buffer, uint32_t sizeBytes)
{
    c->begin = c->current = buffer;
    c->end = (buffer && sizeBytes >= 4u) ? buffer + (sizeBytes / 4u) - 1u : buffer;
    c->measuredWords = 0;
    c->overflowWords = 0;
    c->ended = 0;
}

void FN(SpuSetupMeasureContext)(CellGcmSpuContextData *c)
{
    FN(SpuSetupContextData)(c, NULL, 0);
}

uint32_t *FN(SpuReserve)(CellGcmSpuContextData *c, uint32_t words)
{
    uint32_t *p;
    if (!c->begin) { c->measuredWords += words; return NULL; }
    if (c->ended || words > (uint32_t)(c->end - c->current)) { c->overflowWords += words; return NULL; }
    p = c->current;
    c->current += words;
    return p;
}

uint32_t FN(SpuGetUsedBytes)(const CellGcmSpuContextData *c)
{
    if (!c->begin) return c->measuredWords * 4u;
    return (uint32_t)(c->current - c->begin) * 4u - (c->ended ? 4u : 0u);
}

uint32_t FN(SpuGetOverflowWords)(const CellGcmSpuContextData *c) { return c->overflowWords; }

int32_t FN(SpuEnd)(CellGcmSpuContextData *c)
{
    if (!c->begin || c->ended) return CELL_GCM_SPU_ERROR_STATE;
    *c->current++ = RETURN_WORD;     /* the reserved last word always fits */
    c->ended = 1;
    return c->overflowWords ? CELL_GCM_SPU_ERROR_OVERFLOW : CELL_OK;
}

#define RESERVE(n) uint32_t *w = FN(SpuReserve)(c, (n)); if (!w) return

static void one(CellGcmSpuContextData *c, uint32_t method, uint32_t value)
{
    RESERVE(2);
    w[0] = M(method, 1); w[1] = value;
}

/* ---- flow and labels -------------------------------------------------- */

void FN(SetNopCommand)(CellGcmSpuContextData *c, uint32_t count)
{
    RESERVE(count);
    memset(w, 0, count * 4u);
}
void FN(SetJumpCommand)(CellGcmSpuContextData *c, uint32_t offset) { RESERVE(1); w[0] = offset | JUMP_FLAG; }
void FN(SetCallCommand)(CellGcmSpuContextData *c, uint32_t offset) { RESERVE(1); w[0] = offset | CALL_FLAG; }
void FN(SetReturnCommand)(CellGcmSpuContextData *c) { RESERVE(1); w[0] = RETURN_WORD; }

static void label(CellGcmSpuContextData *c, uint32_t offMethod, uint32_t opMethod, uint8_t index, uint32_t v)
{
    RESERVE(4);
    w[0] = M(offMethod, 1); w[1] = 0x10u * index;
    w[2] = M(opMethod, 1);  w[3] = v;
}
void FN(SetWriteBackEndLabel)(CellGcmSpuContextData *c, uint8_t index, uint32_t value)
{
    /* The back end stores bytes 0 and 2 swapped; pre-swap them. */
    label(c, SEMAPHORE_OFFSET, SEMAPHORE_BACKEND_RELEASE, index,
          (value & 0xff00ff00u) | ((value >> 16) & 0xffu) | ((value & 0xffu) << 16));
}
void FN(SetWriteCommandLabel)(CellGcmSpuContextData *c, uint8_t index, uint32_t value)
{ label(c, CH_SEMAPHORE_OFFSET, CH_SEMAPHORE_RELEASE, index, value); }
void FN(SetWaitLabel)(CellGcmSpuContextData *c, uint8_t index, uint32_t value)
{ label(c, CH_SEMAPHORE_OFFSET, CH_SEMAPHORE_ACQUIRE, index, value); }

/* ---- render state ----------------------------------------------------- */

void FN(SetClearColor)(CellGcmSpuContextData *c, uint32_t color) { one(c, CLEAR_VALUE_COLOR, color); }
void FN(SetClearSurface)(CellGcmSpuContextData *c, uint32_t mask)
{
    RESERVE(6);
    w[0] = M(CLEAR_BUFFERS, 1); w[1] = mask;
    w[2] = M(NOP, 1);           w[3] = 0;
    w[4] = M(WAIT_FOR_IDLE, 1); w[5] = 0;
}
void FN(SetScissor)(CellGcmSpuContextData *c, uint16_t x, uint16_t y, uint16_t wd, uint16_t h)
{
    RESERVE(3);
    w[0] = M(SCISSOR_HORIZ, 2);
    w[1] = ((uint32_t)wd << 16) | x;
    w[2] = ((uint32_t)h << 16) | y;
}
void FN(SetViewport)(CellGcmSpuContextData *c, uint16_t x, uint16_t y, uint16_t wd, uint16_t h,
                     float min, float max, const float scale[4], const float offset[4])
{
    RESERVE(24);
    w[0] = M(VIEWPORT_HORIZ, 2);
    w[1] = ((uint32_t)wd << 16) | x;
    w[2] = ((uint32_t)h << 16) | y;
    w[3] = M(DEPTH_RANGE, 2);
    w[4] = f2u(min); w[5] = f2u(max);
    /* Offset then scale, sent twice. */
    for (int k = 0; k < 2; ++k) {
        uint32_t *p = w + 6 + 9 * k;
        p[0] = M(VIEWPORT_OFFSET, 8);
        for (int i = 0; i < 4; ++i) { p[1 + i] = f2u(offset[i]); p[5 + i] = f2u(scale[i]); }
    }
}
void FN(SetColorMask)(CellGcmSpuContextData *c, uint32_t mask) { one(c, COLOR_MASK, mask); }
void FN(SetColorMaskMrt)(CellGcmSpuContextData *c, uint32_t mask) { one(c, MRT_COLOR_MASK, mask); }
void FN(SetBlendEnable)(CellGcmSpuContextData *c, uint32_t enable) { one(c, BLEND_ENABLE, enable); }
void FN(SetBlendFunc)(CellGcmSpuContextData *c, uint16_t sfcolor, uint16_t dfcolor,
                      uint16_t sfalpha, uint16_t dfalpha)
{
    RESERVE(3);
    w[0] = M(BLEND_FUNC_SRC, 2);
    w[1] = ((uint32_t)sfalpha << 16) | sfcolor;
    w[2] = ((uint32_t)dfalpha << 16) | dfcolor;
}
void FN(SetBlendEquation)(CellGcmSpuContextData *c, uint16_t color, uint16_t alpha)
{ one(c, BLEND_EQUATION, ((uint32_t)alpha << 16) | color); }
void FN(SetDepthTestEnable)(CellGcmSpuContextData *c, uint32_t enable) { one(c, DEPTH_TEST_ENABLE, enable); }
void FN(SetDepthMask)(CellGcmSpuContextData *c, uint32_t enable) { one(c, DEPTH_WRITE_ENABLE, enable); }
void FN(SetDepthFunc)(CellGcmSpuContextData *c, uint32_t func) { one(c, DEPTH_FUNC, func); }
void FN(SetCullFaceEnable)(CellGcmSpuContextData *c, uint32_t enable) { one(c, CULL_FACE_ENABLE, enable); }
void FN(SetFrontFace)(CellGcmSpuContextData *c, uint32_t dir) { one(c, FRONT_FACE, dir); }
void FN(SetPolygonOffsetFillEnable)(CellGcmSpuContextData *c, uint32_t enable)
{ one(c, POLYGON_OFFSET_FILL_ENABLE, enable); }
void FN(SetPolygonOffset)(CellGcmSpuContextData *c, float factor, float units)
{
    RESERVE(3);
    w[0] = M(POLYGON_OFFSET_FACTOR, 2); w[1] = f2u(factor); w[2] = f2u(units);
}

/* ---- vertices and draws ----------------------------------------------- */

void FN(SetVertexDataArray)(CellGcmSpuContextData *c, uint8_t index, uint16_t frequency, uint8_t stride,
                            uint8_t size, uint8_t type, uint8_t location, uint32_t offset)
{
    RESERVE(4);
    w[0] = M(VTXFMT0 + 4u * index, 1);
    w[1] = ((uint32_t)frequency << 16) | ((uint32_t)stride << 8) | ((uint32_t)size << 4) | type;
    w[2] = M(VTXBUF_ADDRESS0 + 4u * index, 1);
    w[3] = ((uint32_t)location << 31) | offset;
}
void FN(SetVertexData4f)(CellGcmSpuContextData *c, uint8_t index, const float v[4])
{
    RESERVE(5);
    w[0] = M(VTX_ATTR_4F_X0 + 16u * index, 4);
    for (int i = 0; i < 4; ++i) w[1 + i] = f2u(v[i]);
}
void FN(SetFrequencyDividerOperation)(CellGcmSpuContextData *c, uint16_t op) { one(c, DIVIDER_FREQUENCY_OP, op); }
void FN(SetInvalidateVertexCache)(CellGcmSpuContextData *c)
{
    RESERVE(8);
    w[0] = M(VTX_CACHE_INVALIDATE2, 1); w[1] = 0;
    for (int i = 0; i < 3; ++i) { w[2 + 2 * i] = M(VTX_CACHE_INVALIDATE, 1); w[3 + 2 * i] = 0; }
}

/* Vertices go in batches of up to 256: one word per batch, (n - 1) << 24 |
 * first.  The first batch rides with BEGIN; the rest follow in
 * non-incrementing packets of up to 2047 full batches. */
void FN(SetDrawArrays)(CellGcmSpuContextData *c, uint32_t mode, uint32_t first, uint32_t count)
{
    uint32_t last = count - 1u, head = last & 0xffu, full = last >> 8;
    uint32_t packets = full / MAX_COUNT, rest = full % MAX_COUNT;
    uint32_t words = 8u + packets * (1u + MAX_COUNT) + (rest ? 1u + rest : 0u) + 2u;
    RESERVE(words);
    *w++ = MNI(VTX_CACHE_INVALIDATE, 3); *w++ = 0; *w++ = 0; *w++ = 0;
    *w++ = M(BEGIN_END, 1); *w++ = mode;
    *w++ = M(VB_VERTEX_BATCH, 1); *w++ = (head << 24) | first;
    first += head + 1u;
    for (uint32_t p = 0; p <= packets; ++p) {
        uint32_t n = p < packets ? MAX_COUNT : rest;
        if (!n) break;
        *w++ = MNI(VB_VERTEX_BATCH, n);
        for (uint32_t i = 0; i < n; ++i, first += 0x100u) *w++ = 0xff000000u | first;
    }
    *w++ = M(BEGIN_END, 1); *w = 0;
}

/* Indices: an optional first batch up to the 128-byte boundary, then
 * batches of 256 indices as above. */
void FN(SetDrawIndexArray)(CellGcmSpuContextData *c, uint8_t mode, uint32_t count, uint8_t type,
                           uint8_t location, uint32_t indices)
{
    uint32_t shift = type == INDEX_TYPE_32 ? 2u : 1u;
    uint32_t mis = (((indices + 127u) & ~127u) - indices) >> shift;
    uint32_t odd = (mis && mis < count) ? 1u : 0u, left = count - (odd ? mis : 0u);
    uint32_t big = left > 0x7ff00u ? (left - 1u) / 0x7ff00u : 0u;   /* full packets */
    uint32_t tail = left - big * 0x7ff00u, tailWords = (tail + 0xffu) >> 8;
    uint32_t words = 7u + 2u * odd + big * (1u + MAX_COUNT) + 1u + tailWords + 2u;
    uint32_t start = 0;
    RESERVE(words);
    *w++ = M(VTX_CACHE_INVALIDATE, 1); *w++ = 0;
    *w++ = M(VB_INDEX_BATCH_OFFSET, 2); *w++ = indices; *w++ = ((uint32_t)type << 4) | location;
    *w++ = M(BEGIN_END, 1); *w++ = mode;
    if (odd) { *w++ = M(VB_INDEX_BATCH_DRAW, 1); *w++ = (mis - 1u) << 24; start = mis; }
    for (uint32_t b = 0; b < big; ++b) {
        *w++ = MNI(VB_INDEX_BATCH_DRAW, MAX_COUNT);
        for (uint32_t i = 0; i < MAX_COUNT; ++i, start += 0x100u) *w++ = 0xff000000u | start;
    }
    *w++ = MNI(VB_INDEX_BATCH_DRAW, tailWords);
    for (; tail > 0x100u; tail -= 0x100u, start += 0x100u) *w++ = 0xff000000u | start;
    if (tail) *w++ = ((tail - 1u) << 24) | start;
    *w++ = M(BEGIN_END, 1); *w = 0;
}

/* ---- programs and constants ------------------------------------------- */

/* `count` floats from register `start`, in packets of 32 floats (8
 * registers) and a remainder. */
void FN(SetVertexProgramConstants)(CellGcmSpuContextData *c, uint32_t start, uint32_t count, const float *data)
{
    uint32_t packets = count >> 5, rest = count & 31u;
    RESERVE(packets * 34u + (rest ? 2u + rest : 0u));
    for (uint32_t p = 0; p < packets; ++p) {
        *w++ = M(VP_UPLOAD_CONST_ID, 33); *w++ = start + 8u * p;
        memcpy(w, data, 32u * 4u); w += 32; data += 32;
    }
    if (rest) {
        *w++ = M(VP_UPLOAD_CONST_ID, rest + 1u); *w++ = start + 8u * packets;
        memcpy(w, data, rest * 4u);
    }
}
void FN(SetVertexProgramParameterBlock)(CellGcmSpuContextData *c, uint32_t baseConst, uint32_t constCount,
                                        const float *values)
{
    if (constCount == 0u || !values) return;
    FN(SetVertexProgramConstants)(c, baseConst, constCount * 4u, values);
}
void FN(SetVertexProgramParameter)(CellGcmSpuContextData *c, const CellGcmSpuVertexParameter *p,
                                   const float *values)
{
    if (!p || !values) return;
    RESERVE(2u + p->words);
    w[0] = M(VP_UPLOAD_CONST_ID, p->words + 1u);
    w[1] = p->resIndex;
    memcpy(w + 2, values, p->words * 4u);
}

static void copy_words(CellGcmSpuContextData *c, const uint32_t *src, uint32_t n)
{
    if (!src) return;
    RESERVE(n);
    memcpy(w, src, n * 4u);
}
void FN(SetVertexProgram)(CellGcmSpuContextData *c, const uint32_t *words, uint32_t n) { copy_words(c, words, n); }
void FN(SetFragmentProgram)(CellGcmSpuContextData *c, const uint32_t *words, uint32_t n) { copy_words(c, words, n); }

void FN(SetUpdateFragmentProgramParameter)(CellGcmSpuContextData *c, uint32_t offset)
{
    one(c, FP_ADDRESS, (0u /* local */ + 1u) | (offset & 0x1fffffffu));
}

/* Each patch is an inline transfer of the constant, with each float's
 * 16-bit halves swapped as the fragment ucode stores them. */
void FN(SetFragmentProgramParameter)(CellGcmSpuContextData *c, const CellGcmSpuFragmentParameter *p,
                                     const float *values, uint32_t addrOffset)
{
    uint32_t total = 0;
    if (!p || !values || !addrOffset || p->patchCount > CELL_GCM_SPU_FP_PARAMETER_MAX_PATCHES) return;
    for (uint32_t i = 0; i < p->patchCount; ++i) total += 12u + ((p->patch[i].words + 1u) & ~1u);
    RESERVE(total);
    for (uint32_t i = 0; i < p->patchCount; ++i) {
        uint32_t dst = addrOffset + p->patch[i].ucodeOffset, n = p->patch[i].words, pad = (n + 1u) & ~1u;
        *w++ = MSUB(3, SURF2D_DMA_IMAGE_DESTIN, 1); *w++ = DMA_MEMORY_FRAME_BUFFER + 0u /* local */;
        *w++ = MSUB(3, SURF2D_OFFSET_DESTIN, 1);    *w++ = dst & ~0x3fu;
        *w++ = MSUB(3, SURF2D_FORMAT, 2);           *w++ = SURF2D_FORMAT_Y32; *w++ = (0x1000u << 16) | 0x1000u;
        *w++ = MSUB(5, IFC_POINT, 3);               *w++ = (dst & 0x3fu) >> 2;
        *w++ = (1u << 16) | n; *w++ = (1u << 16) | n;
        *w++ = MSUB(5, IFC_COLOR0, pad);
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t u = f2u(values[p->patch[i].valueIndex + k]);
            *w++ = (u >> 16) | (u << 16);
        }
        if (pad != n) *w++ = 0;
    }
}

/* ---- textures --------------------------------------------------------- */

void FN(SetTexture)(CellGcmSpuContextData *c, uint8_t index, const CellGcmSpuTexture *t)
{
    uint32_t base = 32u * index;
    if (!t) return;
    RESERVE(9);
    w[0] = M(TEX_OFFSET0 + base, 2);
    w[1] = t->offset;
    w[2] = ((uint32_t)t->location + 1u) | ((uint32_t)t->cubemap << 2) | (1u << 3) /* no border */
         | ((uint32_t)t->dimension << 4) | ((uint32_t)t->format << 8) | ((uint32_t)t->mipmap << 16) | 0x8000u;
    w[3] = M(TEX_SWIZZLE0 + base, 1); w[4] = t->remap;
    w[5] = M(TEX_SIZE0_0 + base, 1);  w[6] = ((uint32_t)t->width << 16) | t->height;
    w[7] = M(TEX_SIZE1_0 + 4u * index, 1); w[8] = ((uint32_t)t->depth << 20) | t->pitch;
}
void FN(SetTextureControl)(CellGcmSpuContextData *c, uint8_t index, uint32_t enable, uint16_t minlod,
                           uint16_t maxlod, uint8_t maxaniso)
{
    one(c, TEX_ENABLE0 + 32u * index, (enable << 31) | ((uint32_t)minlod << 19) | ((uint32_t)maxlod << 7)
                                      | ((uint32_t)maxaniso << 4));
}
void FN(SetTextureFilter)(CellGcmSpuContextData *c, uint8_t index, uint16_t bias, uint8_t min, uint8_t mag,
                          uint8_t conv)
{
    one(c, TEX_FILTER0 + 32u * index, ((uint32_t)mag << 24) | ((uint32_t)min << 16) | ((uint32_t)conv << 13)
                                      | (bias & 0x1fffu));
}
void FN(SetTextureAddress)(CellGcmSpuContextData *c, uint8_t index, uint8_t wraps, uint8_t wrapt, uint8_t wrapr,
                           uint8_t unsignedRemap, uint8_t zfunc, uint8_t gamma)
{
    one(c, TEX_WRAP0 + 32u * index, (uint32_t)wraps | ((uint32_t)wrapt << 8) | ((uint32_t)unsignedRemap << 12)
                                    | ((uint32_t)wrapr << 16) | ((uint32_t)gamma << 20) | ((uint32_t)zfunc << 28));
}
void FN(SetInvalidateTextureCache)(CellGcmSpuContextData *c, uint32_t type) { one(c, TEX_CACHE_CTL, type); }
