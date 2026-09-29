/* cellDmaUnalignedCmd -- DMA transfer with misaligned start/end.
 *
 * The MFC accepts only transfers of 1, 2, 4 or 8 bytes naturally aligned
 * (with matching low four bits of LS and EA), or multiples of 16 bytes
 * with both addresses 16-byte aligned.  A range that starts or ends
 * inside a quadword is therefore split into:
 *   - a head: naturally aligned 1/2/4/8-byte pieces up to the next
 *     16-byte boundary,
 *   - a body: the 16-byte multiple in between, via cellDmaLargeCmd
 *     (which splits it into 16KB commands),
 *   - a tail: naturally aligned 8/4/2/1-byte pieces for the rest.
 * LS and EA must share their low four bits, so aligning EA also aligns
 * LS.  Every piece uses the same tag, so one tag-mask wait covers the
 * whole transfer.
 */

#include <stdint.h>
#include <spu_mfcio.h>

extern void cellDmaLargeCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                            uint32_t tag, uint32_t cmd);

/* Largest of 8/4/2/1 that divides the address and fits in the size. */
static uint32_t piece(uint64_t ea, uint32_t size)
{
    uint32_t p = 8;
    while (p > 1 && ((ea & (p - 1)) != 0 || p > size))
        p >>= 1;
    return p;
}

static void issue(uintptr_t ls, uint64_t ea, uint32_t size,
                  uint32_t tag, uint32_t cmd)
{
    spu_mfcdma64((void *)ls, (unsigned int)(ea >> 32), (unsigned int)ea,
                 size, tag, cmd);
}

void cellDmaUnalignedCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                         uint32_t tag, uint32_t cmd)
{
    uint32_t body;

    while (size > 0 && (ea & 0xf) != 0) {
        uint32_t p = piece(ea, size);
        issue(ls, ea, p, tag, cmd);
        ls += p;
        ea += p;
        size -= p;
    }

    body = size & ~(uint32_t)0xf;
    if (body > 0) {
        cellDmaLargeCmd(ls, ea, body, tag, cmd);
        ls += body;
        ea += body;
        size -= body;
    }

    while (size > 0) {
        uint32_t p = piece(ea, size);
        issue(ls, ea, p, tag, cmd);
        ls += p;
        ea += p;
        size -= p;
    }
}
