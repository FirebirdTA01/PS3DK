/* SPU job-queue runtime, part 6: waking a job suspended with
 * cellSpursJobQueueWaitSignal (cellSpursJobQueueSendSignal).
 *
 * The waiting-job line (CellSpursJobQueueWaitingJob, main memory):
 *   0x00 u32  link in the job queue's resume list
 *   0x28 u32  the job queue EA while the job is suspended
 *   0x2d u8   bit 6: suspended, bit 7: a signal arrived before the job
 *             suspended (its WaitSignal then returns at once)
 *   0x2e u8   resume-list tag, kept in the link's low bits
 * A suspended job goes onto the job queue's resume list (u32 at 0x68, one
 * link per entry); the job queue's workload is then signalled unless
 * 0x66 bit 6 is set or the byte at 0x60 is nonzero.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u

extern int _cellSpursSendWorkloadSignal(unsigned int wid);

static uint8_t wline[128] __attribute__((aligned(128)));
static uint8_t qline[128] __attribute__((aligned(128)));
static uint32_t link[4] __attribute__((aligned(16)));

static inline void get_line(void *ls, uint64_t ea)
{
	mfc_getllar(ls, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static inline int put_line(void *ls, uint64_t ea)
{
	spu_dsync();
	mfc_putllc(ls, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

int cellSpursJobQueueSendSignal(uint64_t eaJob)
{
	uint32_t jq, entry;
	unsigned suspended, tagBits, wake;

	if (!eaJob)
		return (int)JOB_NULL;
	if (eaJob & 0x7f)
		return (int)JOB_ALIGN;

	do {
		uint8_t f;
		get_line(wline, eaJob);
		f = wline[0x2d];
		suspended = (f >> 6) & 1;
		jq = *(volatile uint32_t *)(wline + 0x28);
		tagBits = wline[0x2e];
		wline[0x2d] = (uint8_t)((f & 0x3f) | (suspended ? 0 : 0x80));
		*(volatile uint32_t *)(wline + 0x28) = !suspended;
		if (suspended)
			wline[0x2e] = 0;
	} while (!put_line(wline, eaJob));
	if (!suspended)
		return 0;

	entry = (uint32_t)eaJob + tagBits;
	do {
		get_line(qline, jq);
		link[0] = *(volatile uint32_t *)(qline + 0x68);
		mfc_put(link, eaJob, 4, 0, 0, 0);
		mfc_write_tag_mask(1u << 0);
		(void)mfc_read_tag_status_all();
		*(volatile uint32_t *)(qline + 0x68) = entry;
		wake = (qline[0x66] & 0x40) ? 0 : qline[0x60] == 0;
	} while (!put_line(qline, jq));
	if (wake)
		(void)_cellSpursSendWorkloadSignal(*(volatile uint32_t *)(qline + 0x6c) & 15);
	return 0;
}
