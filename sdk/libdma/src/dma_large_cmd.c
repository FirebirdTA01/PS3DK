/* cellDmaLargeCmd -- issue DMA for transfers exceeding 16KB.
 *
 * The SPU MFC limits a single DMA command to 16KB.  Larger transfers
 * are split into 16KB-aligned chunks, each issued as a separate MFC
 * command with ascending effective addresses and local-store offsets.
 * All chunks share the same tag so a single tag-mask wait covers the
 * entire transfer.
 */
#include <stdint.h>
#include <spu_mfcio.h>

void cellDmaLargeCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                     uint32_t tag, uint32_t cmd)
{
    while (size > 16384) {
        spu_mfcdma64((void *)ls, (unsigned int)(ea >> 32), (unsigned int)ea,
                     16384, tag, cmd);
        ls += 16384;
        ea += 16384;
        size -= 16384;
    }
    if (size > 0)
        spu_mfcdma64((void *)ls, (unsigned int)(ea >> 32), (unsigned int)ea,
                     size, tag, cmd);
}
