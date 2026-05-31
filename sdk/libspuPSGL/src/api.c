/* SPU-side PSGL implementation (independent, reference-faithful). */
#include <PSGL/spu_psgl.h>
#include <cell/dma.h>
#include <spu_intrinsics.h>

/* Forward declarations provided by libgcm_spu.a at link time. */
typedef struct {
    uint32_t *begin, *end, *current;
    uint32_t callback;
} CellGcmContextData;

/* Inline GCM draw-index-array since libgcm_spu.a not yet ported. */
static void cellGcmSetDrawIndexArrayUnsafeInline(CellGcmContextData *ctx,
    uint8_t prim, uint32_t count, uint8_t type, uint8_t loc, uint32_t offset)
{
    uint32_t *c = ctx->current;
    enum { M_BEGIN_END = 0xd4fc, M_INDEX = 0x1718, M_DRAW_ARRAY = 0x172c };
    *c++ = (M_BEGIN_END << 2) | (1u << 18);
    *c++ = prim;
    *c++ = (M_INDEX << 2) | (2u << 18);
    *c++ = (offset & 0x3fffffffu) | ((type & 1u) << 29) | ((loc & 1u) << 30);
    *c++ = offset >> 30;
    *c++ = (M_DRAW_ARRAY << 2) | (1u << 18);
    *c++ = count - 1u;
    ctx->current = c;
}

#define PSGL_DMA_MAX_SIZE 16384u
#define PSGL_IOIF_WSIZE   1024u
#define PSGL_IOIF_NWINDOWS 2u

static unsigned long long g_config[34] __attribute__((aligned(16))) = {0};
static unsigned int g_zero[4] __attribute__((aligned(16))) = {0};

/* map GPU address through IOIF windows */
static unsigned long long psgl_map_gpu(unsigned long long addr, int wid)
{
    addr -= g_config[32];
    const int index = ((int)(addr >> 24) & 0x1c) + wid;
    const unsigned long long offset = addr & 0x3ffffffULL;
    return g_config[index] + offset;
}

/* split instructions that cross 128-byte boundaries */
static uint32_t psgl_patch_stall_sync(uint32_t *cmd, uint32_t size)
{
    uint32_t *start = cmd, *end = cmd + size / 4u;
    do {
        uint32_t get = *cmd;
        uint32_t num = (get >> 18) & 0x7FFu;
        uint32_t *inst_start = cmd, *inst_end = cmd + num;
        if (cmd >= end) break;
        cmd = inst_end + 1;
        if (((uintptr_t)inst_start & ~0x7Fu) == ((uintptr_t)inst_end & ~0x7Fu))
            continue;
        uint32_t inc = ((get >> 28) & 4u) ^ 4u;
        uint32_t *split = (uint32_t *)(((uintptr_t)inst_start + 0x80u) & ~0x7Fu);
        uint32_t new_num = (uint32_t)(split - inst_start - 1);
        if (new_num == 0u) { *inst_start = 0u; }
        else { *inst_start = (get & 0xE003FFFFu) | (new_num << 18); }
        num -= new_num;
        for (uint32_t *cpy = end - 1; cpy >= split; cpy--) cpy[1] = cpy[0];
        *split = ((get & 0xE003FFFFu) | (num << 18)) + inc * new_num;
        cmd = split;
        end++;
        size += 4u;
    } while (1);
    return size;
}

void psglSPUInit(unsigned long long initAddr)
{
    spu_mfcdma64(g_config, (unsigned int)(initAddr >> 32),
                 (unsigned int)(initAddr & 0xffffffffULL),
                 sizeof(g_config), PSGL_READ_TAG, 0x40);
    spu_writech(MFC_WrTagMask, 1u << PSGL_READ_TAG);
    (void)spu_mfcstat(2);
}

void psglSPUWriteMappedBuffer(unsigned long long mappedAddress,
                              const void *localAddress, size_t size)
{
    if ((mappedAddress & g_config[32]) == g_config[32]) {
        int window = 0;
        while (size) {
            unsigned int dma = (size < PSGL_IOIF_WSIZE) ? (unsigned int)size : PSGL_IOIF_WSIZE;
            unsigned long long wa = psgl_map_gpu(mappedAddress, window);
            spu_mfcdma64((void *)localAddress, (unsigned int)(wa >> 32),
                         (unsigned int)(wa & 0xffffffffULL), dma,
                         PSGL_WRITE_TAG, 0x20);
            mappedAddress += dma; localAddress = (const char *)localAddress + dma;
            size -= dma; window = (window + 1) % PSGL_IOIF_NWINDOWS;
        }
    } else {
        /* direct DMA: split into valid MFC shapes. Valid MFC transfer sizes
           are 1/2/4/8/16 bytes or a multiple of 16, and the low 4 bits of the
           EA and LS must match. */
        while (size) {
            unsigned int dma;
            unsigned int ea_off = (unsigned int)((uintptr_t)mappedAddress & 15u);
            unsigned int ls_off = (unsigned int)((uintptr_t)localAddress & 15u);

            /* The low-4-bits-must-match rule is a hard MFC precondition: a
               mismatched EA/LS pair has no legal transfer at any size. The
               JTS hole-fill path always passes matching offsets; if a caller
               violates that, stop rather than emit an illegal DMA. */
            if (ea_off != ls_off)
                break;
            if (ea_off == 0u && size >= 16u) {
                /* 16B-aligned: bulk, largest multiple of 16 up to 16KB */
                dma = ((unsigned int)size > PSGL_DMA_MAX_SIZE)
                    ? PSGL_DMA_MAX_SIZE : (unsigned int)size;
                dma &= ~15u;
            } else {
                /* sub-16: largest of 8/4/2/1 that is NATURALLY aligned to the
                   current offset, fits the remainder, and stays within the
                   16-byte line. Natural alignment (off % dma == 0) is required
                   for MFC element transfers; matching low bits is not enough. */
                dma = 8u;
                while (dma > 1u &&
                       ((ea_off & (dma - 1u)) != 0u ||
                        dma > (unsigned int)size ||
                        ea_off + dma > 16u))
                    dma >>= 1u;
            }

            spu_mfcdma64((void *)localAddress, (unsigned int)(mappedAddress >> 32),
                         (unsigned int)(mappedAddress & 0xffffffffULL), dma,
                         PSGL_WRITE_TAG, 0x20);
            mappedAddress += dma; localAddress = (const char *)localAddress + dma;
            size -= dma;
        }
    }
}

void psglSPUReadMappedBuffer(unsigned long long mappedAddress,
                              void *localAddress, size_t size)
{
    unsigned int max_dma = ((mappedAddress & g_config[32]) == g_config[32])
        ? 16u : PSGL_DMA_MAX_SIZE;
    while (size) {
        unsigned int dma = (size < max_dma) ? (unsigned int)size : max_dma;
        spu_mfcdma64(localAddress, (unsigned int)(mappedAddress >> 32),
                     (unsigned int)(mappedAddress & 0xffffffffULL), dma,
                     PSGL_READ_TAG, 0x40);
        mappedAddress += dma; localAddress = (char *)localAddress + dma;
        size -= dma;
    }
}

int psglFillCommandBufferHole(unsigned int mode, int count, unsigned int type,
                               int isIndexMainMemory, uint32_t indexOffset,
                               uint32_t holeSizeInWord, uint32_t *localHoleBuffer)
{
    /* GL -> GCM primitive translation */
    static const unsigned char gcm_primitive[] = {
        [0x0000] = 0, [0x0001] = 1, [0x0002] = 2, [0x0003] = 3,
        [0x0004] = 4, [0x0005] = 5, [0x0006] = 6, [0x0007] = 7
    };
    uint8_t gcm_mode, gcm_type;
    if (mode > 7u) return 1;
    gcm_mode = gcm_primitive[mode];

    if (type == 0x1405u) gcm_type = 0;      /* GL_UNSIGNED_INT */
    else if (type == 0x1403u) gcm_type = 1;  /* GL_UNSIGNED_SHORT */
    else return 1;

    /* local GCM context */
    CellGcmContextData ctx;
    ctx.begin = localHoleBuffer;
    ctx.current = ctx.begin;
    ctx.end = ctx.begin + holeSizeInWord;
    ctx.callback = 0;

    if (count > 0) {
        uint8_t loc = isIndexMainMemory ? 1u /* CELL_GCM_LOCATION_MAIN */ : 0u;
        cellGcmSetDrawIndexArrayUnsafeInline(&ctx, gcm_mode, (uint32_t)count,
                                              gcm_type, loc, indexOffset);
        uint32_t patched = psgl_patch_stall_sync(ctx.begin,
            (uint32_t)((ctx.current - ctx.begin) * sizeof(uint32_t)));
        ctx.current = ctx.begin + patched / sizeof(uint32_t);
    }

    while (ctx.current < ctx.end)
        *ctx.current++ = 0u;

    return 0;
}

void glSetMappedEventSCE(unsigned long long event)
{
    const int index = (int)((event & 0xcu) >> 2u);
    spu_mfcdma64(&g_zero[index], (unsigned int)(event >> 32),
                 (unsigned int)(event & 0xffffffffULL),
                 sizeof(int), PSGL_WRITE_TAG, 0x22);
}

void glSetMappedEventWithAddressTagSCE(unsigned long long event,
                                        unsigned int *zeroBuffer, unsigned int tag)
{
    const int index = (int)((event & 0xcu) >> 2u);
    spu_mfcdma64(&zeroBuffer[index], (unsigned int)(event >> 32),
                 (unsigned int)(event & 0xffffffffULL),
                 sizeof(int), tag, 0x22);
}
