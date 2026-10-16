/* spurs_dma.h - wait for one DMA tag without disturbing the caller's tag mask.
 *
 * The SPURS task SPU functions run inside the caller task and share its MFC
 * tag mask.  Writing our own mask and leaving it there made the caller's next
 * mfc_read_tag_status_all() wait on OUR tag instead of its own, so it read
 * LS before its DMA had landed: on a real PS3 the spurs-jq-spu job computed a
 * suspended-job size from the previous descriptor (RPCS3 completes DMA
 * immediately and never showed it).  Save the caller's mask, wait, restore.
 */
#ifndef PS3DK_LIBSPURS_TASK_DMA_H
#define PS3DK_LIBSPURS_TASK_DMA_H

#include <spu_mfcio.h>

static inline void spurs_dma_wait(unsigned tag)
{
	const unsigned saved = mfc_read_tag_mask();
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
	mfc_write_tag_mask(saved);
}

#endif
