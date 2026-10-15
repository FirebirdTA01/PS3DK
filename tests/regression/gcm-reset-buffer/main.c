/* gcm-reset-buffer: cellGcmResetDefaultCommandBufferUnsafe (rsxResetCommand-
 * Buffer underneath) sends the RSX back to the start of the default command
 * buffer and must rewind the context's write pointer to match.
 *
 * Writes commands, resets, then checks that context->current is back at
 * context->begin, that GET sits at begin's offset, and that a label written
 * after the reset still completes.  Prints GCM_RESET_OK or GCM_RESET_FAIL. */
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/process.h>
#include <cell/gcm.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (1024 * 1024)
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
	void *host = memalign(1024 * 1024, HOST_SIZE);
	if (!host || cellGcmInit(CB_SIZE, HOST_SIZE, host) != 0) {
		printf("GCM_RESET_FAIL init\n");
		return 1;
	}
	CellGcmContextData *ctx = gCellGcmCurrentContext;
	uint32_t begin_off = 0;
	cellGcmAddressToOffset(ctx->begin, &begin_off);
	volatile uint32_t *label = cellGcmGetLabelAddress(LABEL_INDEX);

	/* Move the write pointer well into the buffer and let the RSX drain it. */
	*label = 0;
	for (uint32_t i = 0; i < 200; i++)
		cellGcmSetReferenceCommand(ctx, i);
	cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, 1);
	cellGcmFlush(ctx);
	if (!wait_label(label, 1))
		fails++, printf("GCM_RESET_FAIL pre-reset label never landed\n");
	long used = (long)(ctx->current - ctx->begin);
	printf("gcm-reset-buffer: begin offset 0x%x, %ld words written before reset\n", (unsigned)begin_off, used);

	cellGcmResetDefaultCommandBufferUnsafe(ctx);

	CellGcmControl volatile *ctrl = cellGcmGetControlRegister();
	long after = (long)(ctx->current - ctx->begin);
	if (after != 0)
		fails++, printf("GCM_RESET_FAIL current is %ld words past begin after the reset\n", after);
	if (ctrl->get != begin_off)
		fails++, printf("GCM_RESET_FAIL GET 0x%x after the reset, begin offset 0x%x\n",
		                (unsigned)ctrl->get, (unsigned)begin_off);

	/* Commands written after the reset still execute. */
	cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, 2);
	cellGcmFlush(ctx);
	if (!wait_label(label, 2))
		fails++, printf("GCM_RESET_FAIL post-reset label never landed\n");

	if (fails)
		printf("GCM_RESET_FAIL %d\n", fails);
	else
		printf("GCM_RESET_OK\n");
	return fails != 0;
}
