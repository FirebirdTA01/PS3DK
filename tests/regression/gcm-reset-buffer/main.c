/* gcm-reset-buffer: cellGcmResetDefaultCommandBufferUnsafe (rsxResetCommand-
 * Buffer underneath) and its global form cellGcmResetDefaultCommandBuffer()
 * send the RSX back to the start of the default command buffer and must
 * rewind the context's write pointer to match.
 *
 * Twice, once per form: writes commands, resets, then checks that
 * context->current is back at context->begin, that GET sits at begin's
 * offset, and that a label written after the reset still completes.  Prints
 * GCM_RESET_OK or GCM_RESET_FAIL. */
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <unistd.h>

#include "../../../samples/gcm/common/gcm_host_buffer.h"

#include <sys/process.h>
#include <cell/gcm.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define LABEL_INDEX 254

static int fails;

static int wait_label(volatile uint32_t *label, uint32_t value)
{
	int i;
	for (i = 0; i < 20000 && *label != value; i++)
		usleep(100);
	return *label == value;
}

int main(void)
{
	sys_addr_t host = 0;
	/* LV2 pages, not the sbrk heap (see gcm_host_buffer.h).  Never freed
	 * after GCM init — process exit reclaims it. */
	if (gcm_host_buffer_alloc(HOST_SIZE, &host) != 0) {
		printf("GCM_RESET_FAIL gcm_host_buffer_alloc failed\n");
		return 1;
	}
	if (cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host) != 0) {
		printf("GCM_RESET_FAIL 1 (init failed)\n");
		return 1;
	}
	CellGcmContextData *ctx = gCellGcmCurrentContext;
	uint32_t begin_off = 0;
	cellGcmAddressToOffset(ctx->begin, &begin_off);
	volatile uint32_t *label = cellGcmGetLabelAddress(LABEL_INDEX);

	for (int round = 0; round < 2; round++) {
		const char *form = round == 0 ? "context form" : "global form";
		uint32_t pre = 1 + 2 * round, post = 2 + 2 * round;

		/* Move the write pointer well into the buffer and let the RSX drain it. */
		*label = 0;
		for (uint32_t i = 0; i < 200; i++)
			cellGcmSetReferenceCommand(ctx, i);
		cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, pre);
		cellGcmFlush(ctx);
		if (!wait_label(label, pre))
			fails++, printf("GCM_RESET_FAIL %s: pre-reset label never landed\n", form);
		long used = (long)(ctx->current - ctx->begin);
		printf("gcm-reset-buffer: %s: begin offset 0x%x, %ld words written before reset\n", form, (unsigned)begin_off,
		       used);

		if (round == 0)
			cellGcmResetDefaultCommandBufferUnsafe(ctx);
		else
			cellGcmResetDefaultCommandBuffer();

		CellGcmControl volatile *ctrl = cellGcmGetControlRegister();
		long after = (long)(ctx->current - ctx->begin);
		if (after != 0)
			fails++, printf("GCM_RESET_FAIL %s: current is %ld words past begin after the reset\n", form, after);
		if (ctrl->get != begin_off)
			fails++, printf("GCM_RESET_FAIL %s: GET 0x%x after the reset, begin offset 0x%x\n", form,
			                (unsigned)ctrl->get, (unsigned)begin_off);

		/* Commands written after the reset still execute. */
		cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, post);
		cellGcmFlush(ctx);
		if (!wait_label(label, post))
			fails++, printf("GCM_RESET_FAIL %s: post-reset label never landed\n", form);
	}

	if (fails)
		printf("GCM_RESET_FAIL %d\n", fails);
	else
		printf("GCM_RESET_OK\n");
	return fails != 0;
}
