/*
 * Every cellDma* name in <cell/dma.h>, used once with valid arguments,
 * compiled for the SPU as C and C++ under -Wall -Wextra -Werror in each
 * check mode (default, NO_CELL_DMA_ASSERT, CELL_DMA_ASSERT_VERBOSE) and
 * linked against libdma.  This proves the surface declares and links; it
 * does not execute the transfers.
 */
#include <stdint.h>
#include <cell/dma.h>

static uint8_t line[128] __attribute__((aligned(128)));
static uint8_t data[32768] __attribute__((aligned(128)));
static CellDmaListElement list[4] __attribute__((aligned(8)));

int main(uint64_t ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	uint32_t tag = 3, sum = 0;
	(void)arg2; (void)arg3; (void)arg4;
	ea &= ~(uint64_t)0x7f;

	cellDmaGet(data, ea, 128, tag, 0, 0);
	cellDmaGetf(data, ea, 128, tag, 0, 0);
	cellDmaGetb(data, ea, 128, tag, 0, 0);
	cellDmaPut(data, ea, 128, tag, 0, 0);
	cellDmaPutf(data, ea, 128, tag, 0, 0);
	cellDmaPutb(data, ea, 128, tag, 0, 0);
	cellDmaListGet(data, ea, list, sizeof list, tag, 0, 0);
	cellDmaListGetf(data, ea, list, sizeof list, tag, 0, 0);
	cellDmaListGetb(data, ea, list, sizeof list, tag, 0, 0);
	cellDmaListPut(data, ea, list, sizeof list, tag, 0, 0);
	cellDmaListPutf(data, ea, list, sizeof list, tag, 0, 0);
	cellDmaListPutb(data, ea, list, sizeof list, tag, 0, 0);

	cellDmaSmallGet(data + 4, ea + 4, 4, tag, 0, 0);
	cellDmaSmallGetf(data + 8, ea + 8, 8, tag, 0, 0);
	cellDmaSmallGetb(data + 2, ea + 2, 2, tag, 0, 0);
	cellDmaSmallPut(data + 1, ea + 1, 1, tag, 0, 0);
	cellDmaSmallPutf(data + 4, ea + 4, 4, tag, 0, 0);
	cellDmaSmallPutb(data + 8, ea + 8, 8, tag, 0, 0);

	cellDmaLargeGet(data, ea, sizeof data, tag, 0, 0);
	cellDmaLargeGetf(data, ea, sizeof data, tag, 0, 0);
	cellDmaLargeGetb(data, ea, sizeof data, tag, 0, 0);
	cellDmaLargePut(data, ea, sizeof data, tag, 0, 0);
	cellDmaLargePutf(data, ea, sizeof data, tag, 0, 0);
	cellDmaLargePutb(data, ea, sizeof data, tag, 0, 0);

	cellDmaUnalignedGet(data + 3, ea + 3, 1000, tag, 0, 0);
	cellDmaUnalignedGetf(data + 3, ea + 3, 1000, tag, 0, 0);
	cellDmaUnalignedGetb(data + 3, ea + 3, 1000, tag, 0, 0);
	cellDmaUnalignedPut(data + 3, ea + 3, 1000, tag, 0, 0);
	cellDmaUnalignedPutf(data + 3, ea + 3, 1000, tag, 0, 0);
	cellDmaUnalignedPutb(data + 3, ea + 3, 1000, tag, 0, 0);

	cellDmaAndWait((uintptr_t)data, ea, 128, tag, MFC_CMD_WORD(0, 0, MFC_GET_CMD));
	cellDmaLargeCmd((uintptr_t)data, ea, sizeof data, tag, MFC_CMD_WORD(0, 0, MFC_GET_CMD));
	cellDmaUnalignedCmd((uintptr_t)data + 5, ea + 5, 77, tag, MFC_CMD_WORD(0, 0, MFC_GET_CMD));

	sum += cellDmaGetUint8(ea + 1, tag, 0, 0);
	sum += cellDmaGetUint16(ea + 2, tag, 0, 0);
	sum += cellDmaGetUint32(ea + 4, tag, 0, 0);
	sum += (uint32_t)cellDmaGetUint64(ea + 8, tag, 0, 0);
	sum += cellDmaGetUintTemplate(32, ea + 12, tag, 0, 0);
	cellDmaPutUint8(1, ea + 1, tag, 0, 0);
	cellDmaPutUint16(2, ea + 2, tag, 0, 0);
	cellDmaPutUint32(3, ea + 4, tag, 0, 0);
	cellDmaPutUint64(4, ea + 8, tag, 0, 0);
	cellDmaPutUintTemplate(32, sum, ea + 12, tag, 0, 0);

	cellDmaGetllar(line, ea, 0, 0);
	sum += cellDmaWaitAtomicStatus();
	cellDmaPutllc(line, ea, 0, 0);
	sum += cellDmaWaitAtomicStatus();
	cellDmaPutlluc(line, ea, 0, 0);
	sum += cellDmaWaitAtomicStatus();
	cellDmaPutqlluc(line, ea, tag, 0, 0);

	sum += (uint32_t)cellDmaEa2Ls(ea + 5, data);
	sum += cellDmaWaitTagStatusAll(1u << tag);
	sum += cellDmaWaitTagStatusAny(1u << tag);
	sum += cellDmaWaitTagStatusImmediate(1u << tag);
	sum += cellDmaTagStatusAll(1u << tag);
	sum += cellDmaGetTagStatus();
	sum += cellDmaGetUnusedTagStatus(1u << tag);
	cellDmaCancelTagStatusUpdate();
	sum += cellDmaCancelAndWaitTagStatusAny(1u << tag);
	sum += cellDmaCancelAndWaitTagStatusAll(1u << tag);

	cellDmaAssert(sum != 1, "sum");
	cellDmaNormalAssert(data, ea, 128, tag);
	cellDmaSmallAssert(data + 4, ea + 4, 4, tag);
	cellDmaListAssert(data, ea, list, sizeof list, tag);
	cellDmaAtomicAssert(line, ea);
	cellDmaPutqllucAssert(line, ea, tag);
	cellDmaDataAssert(ea + 4, 4, tag);
	cellDmaLargeAssert(data, ea, tag);
	cellDmaUnalignedAssert(data + 3, ea + 3, tag);
	return (int)(sum & 0xff);
}
