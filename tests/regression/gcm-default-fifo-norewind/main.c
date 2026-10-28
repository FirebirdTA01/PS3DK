/* gcm-default-fifo-norewind: gcmSetDefaultFifoSize, called after cellGcmInit
 * exactly as libPSGL does, must not move the live FIFO cursor.
 *
 * After cellGcmInit the firmware has already published and fetched its
 * initialization words (PUT == GET past begin) and two more words are queued
 * at current.  Rewinding current to begin makes the next flush publish PUT
 * behind GET with no JUMP and no wrap: an unpaired rewind.  GET > PUT alone is
 * not a fault (a legitimate wrap produces it), so the check requires that no
 * wrap happened (wrap count 0) and that current moved backwards.
 *
 * Checks: the call succeeds; begin, current and the queued words are kept;
 * the first flush after it does not leave PUT behind GET without a wrap; a
 * reference update and a back-end label written afterwards complete.
 * Prints GCM_NOREWIND_OK checks=N failures=0 or GCM_NOREWIND_FAIL lines.
 * RPCS3 diagnostic: on silicon an unpaired rewind can wedge the RSX. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include "../../../samples/gcm/common/gcm_host_buffer.h"

#include <sys/process.h>
#include <cell/gcm.h>
#include <cell/gcm/ps3tc_fifo_wrap.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define CB_SIZE     0x100000u          /* libPSGL's default FIFO size */
#define HOST_SIZE   (32u * 1024u * 1024u)
#define SEGMENT     16384u             /* libPSGL's 64 KiB segment, in words */
#define LABEL_INDEX 253

static int checks, failures;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
		printf("GCM_NOREWIND_FAIL %s\n", what);
	}
}

static int wait_word(volatile uint32_t *word, uint32_t value)
{
	for (int i = 0; i < 50000 && *word != value; i++)
		usleep(100);
	return *word == value;
}

int main(void)
{
	sys_addr_t host = 0;
	/* LV2 pages, not the sbrk heap (see gcm_host_buffer.h).  Never freed
	 * after GCM init — process exit reclaims it. */
	if (gcm_host_buffer_alloc(HOST_SIZE, &host) != 0) {
		printf("GCM_NOREWIND_FAIL gcm_host_buffer_alloc failed\n");
		return 1;
	}
	if (cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host) != 0) {
		printf("GCM_NOREWIND_FAIL init failed\n");
		return 1;
	}
	CellGcmContextData *ctx = gCellGcmCurrentContext;
	CellGcmControl volatile *ctrl = cellGcmGetControlRegister();
	uint32_t begin_off = 0, cur_off = 0;
	cellGcmAddressToOffset(ctx->begin, &begin_off);
	cellGcmAddressToOffset(ctx->current, &cur_off);
	uint32_t *begin0 = ctx->begin, *current0 = ctx->current;
	uint32_t queued0[2] = { current0[-2], current0[-1] };
	printf("gcm-default-fifo-norewind: after init begin 0x%x current 0x%x put 0x%x get 0x%x\n",
	       (unsigned)begin_off, (unsigned)cur_off, (unsigned)ctrl->put, (unsigned)ctrl->get);

	int rc = gcmSetDefaultFifoSize(CB_SIZE / 4u, SEGMENT);
	check(rc == 0, "gcmSetDefaultFifoSize refused the admitted size");
	check(ctx->begin == begin0, "begin moved");
	check(ctx->current == current0, "current moved (rewind)");
	check(current0[-2] == queued0[0] && current0[-1] == queued0[1], "queued words changed");

	uint64_t wraps0 = 0, wraps1 = 0;
	ps3tc_fifo_wrap_stats(&wraps0, NULL, NULL);
	uint32_t token = 0x5a5a1234u;
	cellGcmSetReferenceCommand(ctx, token);
	cellGcmFlush(ctx);
	uint32_t put = ctrl->put, get = ctrl->get;
	ps3tc_fifo_wrap_stats(&wraps1, NULL, NULL);
	printf("gcm-default-fifo-norewind: first flush put 0x%x get 0x%x wraps %llu\n",
	       (unsigned)put, (unsigned)get, (unsigned long long)(wraps1 - wraps0));
	check(!(put < get && wraps1 == wraps0), "unpaired rewind: PUT behind GET with no wrap");
	check(wait_word((volatile uint32_t *)&ctrl->ref, token), "reference update did not complete");

	volatile uint32_t *label = cellGcmGetLabelAddress(LABEL_INDEX);
	uint32_t value = *label == 0x13572468u ? 0x13572469u : 0x13572468u;
	cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, value);
	cellGcmFlush(ctx);
	check(wait_word(label, value), "back-end label did not complete");

	if (failures == 0)
		printf("GCM_NOREWIND_OK checks=%d failures=0\n", checks);
	else
		printf("gcm-default-fifo-norewind: checks=%d failures=%d\n", checks, failures);
	return failures ? 1 : 0;
}
