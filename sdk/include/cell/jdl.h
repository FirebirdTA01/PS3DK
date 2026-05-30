#ifndef __PS3DK_CELL_JDL_H__
#define __PS3DK_CELL_JDL_H__

#include <stdint.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_queue_types.h>

#define _JDL_STR(x) #x

#ifndef JDL_ASSERT
# if defined(NDEBUG) || defined(CELL_JDL_NDEBUG)
#  define JDL_ASSERT(expr) ((void)0)
# elif defined(__SPU__)
#  include <spu_printf.h>
#  define JDL_ASSERT(expr)                                                   \
    do {                                                                     \
        if (!(expr)) {                                                       \
            spu_printf("%s:%d " _JDL_STR(expr) " -- assertion failed\n",    \
                       __FILE__, __LINE__);                                  \
            __asm__ volatile("stopd $0,$0,$0\n");                            \
        }                                                                    \
    } while (0)
# else
#  include <assert.h>
#  define JDL_ASSERT(expr) assert(expr)
# endif
#endif

#ifndef JDL_WARN
# if defined(NDEBUG) || defined(CELL_JDL_NDEBUG) || defined(CELL_JDL_NWARN)
#  define JDL_WARN(expr, message) ((void)0)
# elif defined(__SPU__)
#  include <spu_printf.h>
#  define JDL_WARN(expr, message)                                            \
    do {                                                                     \
        if (expr) {                                                          \
            spu_printf(message);                                             \
        }                                                                    \
    } while (0)
# else
#  include <stdio.h>
#  define JDL_WARN(expr, message)                                            \
    do {                                                                     \
        if (expr) {                                                          \
            printf(message);                                                 \
            fflush(stdout);                                                  \
        }                                                                    \
    } while (0)
# endif
#endif

#ifndef JDL_MIN
# define JDL_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define CELL_JDL_CHECK_BUFFER_SMALL(jobName, bufName, maxSize, eal, size)    \
    JDL_ASSERT(((eal) & 0xf0000000u) != 0xd0000000u &&                       \
               "buffer " #bufName " must not be in PPU stack memory");      \
    JDL_ASSERT(((eal) & ((size) - 1u)) == 0 &&                               \
               "buffer " #bufName " is not aligned properly for DMA")

#define CELL_JDL_CHECK_BUFFER_ANY(jobName, bufName, maxSize, eal, size, alignment) \
    JDL_ASSERT(((eal) & 0xf0000000u) != 0xd0000000u &&                       \
               "buffer " #bufName " must not be in PPU stack memory");      \
    JDL_ASSERT(((eal) & ((alignment) - 1u)) == 0 &&                          \
               "buffer " #bufName " is not aligned properly");              \
    JDL_ASSERT(((size) & ((alignment) - 1u)) == 0 &&                         \
               "buffer " #bufName " size is not aligned properly");         \
    JDL_ASSERT((size) <= (maxSize) &&                                        \
               "buffer " #bufName " exceeds maximum size")

#define CELL_JDL_CHECK_BUFFER(jobName, bufName, maxSize, eal, size, alignment) \
    do {                                                                     \
        CELL_JDL_CHECK_BUFFER_ANY(jobName, bufName, maxSize, eal, size, alignment); \
    } while (0)

#ifndef CELL_JDL_CHECK_HEADER_POINTER
# ifdef __PPU__
#  define CELL_JDL_CHECK_HEADER_POINTER(jobName, header)                     \
    JDL_WARN((((uint32_t)(uintptr_t)&(header)) & 0xf0000000u) == 0xd0000000u, \
             "Warning: job descriptor must not be used from PPU stack memory")
# else
#  define CELL_JDL_CHECK_HEADER_POINTER(jobName, header) ((void)0)
# endif
#endif

#ifdef __SPU__

#include <alloca.h>
#include <spu_mfcio.h>

#define JDL_SAFE_ALLOCA(size, align)                                         \
    ((void *)((((uintptr_t)alloca((size) + (align) - 1u)) + (align) - 1u) &  \
              ~((uintptr_t)((align) - 1u))))

static inline void _jdl_mfc_any(void *ls, uint64_t ea, uint32_t size,
                                uint32_t tag, uint32_t cmd)
{
    spu_mfcdma64(ls, mfc_ea2h(ea), mfc_ea2l(ea), size, tag,
                 MFC_CMD_WORD(0, 0, cmd));
}

static inline void _jdl_mfc_put_large(void *ls, uint64_t ea, uint32_t size,
                                      uint32_t tag, uint32_t dma_cmd)
{
    uintptr_t local = (uintptr_t)ls;

    while (size > 0) {
        uint32_t chunk = JDL_MIN(size, (uint32_t)MFC_MAX_DMA_SIZE);
        _jdl_mfc_any((void *)local, ea, chunk, tag, dma_cmd);
        local += chunk;
        ea += chunk;
        size -= chunk;
    }
}

static inline uint32_t _jdl_dma_unaligned_chunk(uintptr_t local,
                                                uint32_t size,
                                                uint32_t align_amount)
{
    if (align_amount <= 1u && (local & 1u) != 0u) {
        return 1u;
    }
    if (align_amount <= 2u && (local & 2u) != 0u && size >= 2u) {
        return 2u;
    }
    if (align_amount <= 4u && (local & 4u) != 0u && size >= 4u) {
        return 4u;
    }
    if (align_amount <= 8u && (local & 8u) != 0u && size >= 8u) {
        return 8u;
    }
    if (size >= MFC_MAX_DMA_SIZE) {
        return MFC_MAX_DMA_SIZE;
    }
    if (size >= MFC_MIN_DMA_SIZE) {
        return size & ~((uint32_t)MFC_MIN_DMA_SIZE_MASK);
    }
    if (size >= 8u) {
        return 8u;
    }
    if (size >= 4u) {
        return 4u;
    }
    if (size >= 2u && align_amount <= 2u) {
        return 2u;
    }
    return 1u;
}

static inline void _jdl_mfc_put_large_unaligned(void *ls, uint64_t ea,
                                                uint32_t size, uint32_t tag,
                                                uint32_t dma_cmd
# ifdef __cplusplus
                                                , uint32_t align_amount = 1u
# else
                                                , uint32_t align_amount
# endif
                                                )
{
    uintptr_t local = (uintptr_t)ls;

    if (align_amount == 0u) {
        align_amount = 1u;
    }

    while (size > 0) {
        uint32_t chunk = _jdl_dma_unaligned_chunk(local, size, align_amount);
        _jdl_mfc_any((void *)local, ea, chunk, tag, dma_cmd);
        local += chunk;
        ea += chunk;
        size -= chunk;
    }
}

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_JDL_H__ */
