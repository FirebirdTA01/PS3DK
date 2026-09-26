/* lockline.c -- heap lock and header-line transfers.
 *
 * The heap lock is word 0 of the 128-byte header line.  It is taken with a
 * line reservation (getllar, then putllc of the line with the word set to
 * 1) and released with an unconditional line store (putlluc) of the
 * holder's snapshot with the word cleared.  The rest of the line never
 * changes after initialisation, so writing the snapshot back is harmless.
 * The PPU side takes the same word with lwarx/stwcx.; on Cell the PPU
 * reservation granule is the same 128-byte line, so the two interoperate.
 */
#include <spu_mfcio.h>
#include "sheap_spu.h"

static void line_reserve(void *ls, uint64_t ea)
{
    SHEAP_DMA_BARRIER();
    mfc_getllar(ls, ea, 0, 0);
    (void)mfc_read_atomic_status();
    SHEAP_DMA_BARRIER();
}

static int line_store_conditional(void *ls, uint64_t ea)
{
    SHEAP_DMA_BARRIER();
    mfc_putllc(ls, ea, 0, 0);
    return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

void __sheap_lock(uint64_t ea, sheap_header *line)
{
    for (;;) {
        line_reserve(line, ea);
        if (line->lock != 0)
            continue;               /* held elsewhere: spin on the line */
        line->lock = 1;
        if (line_store_conditional(line, ea))
            return;
    }
}

void __sheap_unlock(uint64_t ea, sheap_header *line)
{
    line->lock = 0;
    __sheap_publish(ea, line);
}

void __sheap_fetch(uint64_t ea, sheap_header *line)
{
    do {
        line_reserve(line, ea);
    } while (!line_store_conditional(line, ea));
}

void __sheap_publish(uint64_t ea, sheap_header *line)
{
    SHEAP_DMA_BARRIER();
    mfc_putlluc(line, ea, 0, 0);
    (void)mfc_read_atomic_status();
    SHEAP_DMA_BARRIER();
}

void __sheap_dma_wait(unsigned tag)
{
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
    SHEAP_DMA_BARRIER();
}

/* Source for zero-filling: 1 KB of BSS, never written. */
static uint8_t zero_block[1024] __attribute__((aligned(128)));

void __sheap_dma_zero(uint64_t ea, uint64_t size, unsigned tag)
{
    while (size != 0) {
        uint32_t chunk = size > sizeof(zero_block) ? (uint32_t)sizeof(zero_block)
                                                   : (uint32_t)size;

        mfc_put(zero_block, ea, chunk, tag, 0, 0);
        ea += chunk;
        size -= chunk;
    }
    __sheap_dma_wait(tag);
}
