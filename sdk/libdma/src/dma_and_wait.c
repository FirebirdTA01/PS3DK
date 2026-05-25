/* cellDmaAndWait -- issue MFC DMA command and spin-wait for completion.
 *
 * Cell BE Handbook, Chapter 18: MFC transfers are tracked by tag;
 * after issuing the command, the SPU polls mfc_read_tag_status_all()
 * until the matching tag bit clears (transfer complete).
 */
#include <stdint.h>
#include <spu_mfcio.h>

void cellDmaAndWait(uintptr_t ls, uint64_t ea, uint32_t size,
                    uint32_t tag, uint32_t cmd)
{
    uint32_t mask;
    spu_mfcdma64((void *)ls, (unsigned int)(ea >> 32), (unsigned int)ea,
                 size, tag, cmd);
    mask = 1u << tag;
    mfc_write_tag_mask(mask);
    mfc_read_tag_status_all();
}
