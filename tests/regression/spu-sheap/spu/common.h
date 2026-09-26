/* spu-sheap regression rows, SPU side: shared helpers. */
#ifndef SPU_SHEAP_COMMON_H
#define SPU_SHEAP_COMMON_H

#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <cell/sheap.h>

/* DMA tag for the programs' own transfers (the heap uses its own tag). */
#define ROW_TAG 20

static uint64_t row_result[16] __attribute__((aligned(128)));
static int row_failed;

/* Record the first failed check; the exit status reports it. */
#define CHECK(n, cond) do { if (!(cond) && !row_failed) row_failed = (n); } while (0)

static inline void row_dma_wait(void)
{
    mfc_write_tag_mask(1u << ROW_TAG);
    mfc_read_tag_status_all();
}

static inline void row_get(volatile void *ls, uint64_t ea, uint32_t size)
{
    mfc_get(ls, ea, size, ROW_TAG, 0, 0);
    row_dma_wait();
}

static inline void row_put(volatile void *ls, uint64_t ea, uint32_t size)
{
    mfc_put(ls, ea, size, ROW_TAG, 0, 0);
    row_dma_wait();
}

/* Send row_result to the PPU's 128-byte result block, then exit. */
static inline void row_finish(uint64_t ea_result)
{
    if (ea_result)
        row_put(row_result, ea_result, sizeof(row_result));
    sys_spu_thread_exit(row_failed);
}

/* A 32-bit word in main memory, read or written through a 16-byte slot at
 * the same offset modulo 16 (DMA rule for sub-quadword transfers). */
static inline uint32_t row_get32(uint64_t ea)
{
    static uint32_t slot[4] __attribute__((aligned(16)));

    row_get(&slot[(ea & 15) >> 2], ea, 4);
    return slot[(ea & 15) >> 2];
}

static inline void row_put32(uint64_t ea, uint32_t value)
{
    static uint32_t slot[4] __attribute__((aligned(16)));

    slot[(ea & 15) >> 2] = value;
    row_put(&slot[(ea & 15) >> 2], ea, 4);
}

#endif /* SPU_SHEAP_COMMON_H */
