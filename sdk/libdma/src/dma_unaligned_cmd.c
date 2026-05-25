/* cellDmaUnalignedCmd -- DMA transfer with misaligned start/end.
 *
 * When the EA and LS addresses have identical low-4-bit offsets but
 * are not 16-byte aligned, a single DMA cannot cover the full range.
 * The transfer is split into up to three segments: a small head chunk
 * to align to 16 bytes, aligned 16KB chunks via cellDmaLargeCmd, and
 * a small tail chunk.
 */
#include <stdint.h>
#include <spu_mfcio.h>

extern void cellDmaLargeCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                            uint32_t tag, uint32_t cmd);

void cellDmaUnalignedCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                         uint32_t tag, uint32_t cmd)
{
    uint32_t offset = (uint32_t)ea & 0xf;
    uint32_t head;

    if (offset != 0 && size > 0) {
        head = 16 - offset;
        if (head > size) head = size;
        spu_mfcdma64((void *)ls, (unsigned int)(ea >> 32), (unsigned int)ea,
                     head, tag, cmd);
        ls += head;
        ea += head;
        size -= head;
    }

    if (size > 16384) {
        cellDmaLargeCmd(ls, ea, size, tag, cmd);
        return;
    }

    if (size > 0)
        spu_mfcdma64((void *)ls, (unsigned int)(ea >> 32), (unsigned int)ea,
                     size, tag, cmd);
}
