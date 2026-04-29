// hello-spu-dma — SPU side.
//
// Validates the SPU-side <cell/dma.h> wrapper surface:
//   - cellDmaGet  + cellDmaWaitTagStatusAll : blocking fetch
//   - cellDmaListGet                        : 2-element split fetch
//   - cellDmaPut                            : write-back
//   - CellDmaListElement                    : packed (size, eal) layout
//
// Argument convention (from PPU sysSpuThreadArgument):
//   arg1 = EA of output buffer  (16 uint32)
//   arg2 = EA of input  buffer  (16 uint32)
//   arg3 = EA of done   word    (uint32)
//   arg4 = element count (always 16 here; ignored)
//
// Sizes are kept literal so GCC can pick the SIMD-vectorised loop
// shape that the RPCS3 SPU LLVM recompiler is happy with — variable
// sizes scalarise the doubling loop and trip a recompiler edge case
// in the build under test.

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include <cell/dma.h>

#define DMA_TAG_GET      1
#define DMA_TAG_LIST     2
#define DMA_TAG_PUT      3
#define DMA_TAG_DONE     4

static __attribute__((aligned(128))) uint32_t buf   [16];
static __attribute__((aligned(128))) uint32_t verify[16];
static __attribute__((aligned(8)))   CellDmaListElement list[2];
static __attribute__((aligned(16)))  uint32_t done_buf[4] = { 0xc0ffee, 0, 0, 0 };

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    uint64_t out_ea  = arg1;
    uint64_t in_ea   = arg2;
    uint64_t flag_ea = arg3;
    (void)arg4;

    /* Path 1: simple cellDmaGet of the whole input buffer. */
    cellDmaGet(buf, in_ea, 64, DMA_TAG_GET, 0, 0);
    cellDmaWaitTagStatusAll(1u << DMA_TAG_GET);

    /* Path 2: cellDmaListGet — split the same fetch into two halves
       to exercise the DMA-list path. CellDmaListElement layout
       (notify:1 reserved:16 size:15 eal:32). */
    list[0].size = 32;
    list[0].eal  = (uint32_t)in_ea;
    list[1].size = 32;
    list[1].eal  = (uint32_t)in_ea + 32;

    cellDmaListGet(verify, in_ea, list, sizeof(list),
                   DMA_TAG_LIST, 0, 0);
    cellDmaWaitTagStatusAll(1u << DMA_TAG_LIST);

    /* Cross-check the two paths agree, then double in place. */
    int mismatch = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (buf[i] != verify[i]) mismatch = 1;
    }
    if (mismatch) {
        for (uint32_t i = 0; i < 16; ++i) buf[i] = 0xbad0bad0u;
    } else {
        for (uint32_t i = 0; i < 16; ++i) buf[i] = buf[i] * 2;
    }

    /* Write back. */
    cellDmaPut(buf, out_ea, 64, DMA_TAG_PUT, 0, 0);
    cellDmaWaitTagStatusAll(1u << DMA_TAG_PUT);

    /* Signal done. */
    cellDmaPut(done_buf, flag_ea, 16, DMA_TAG_DONE, 0, 0);
    cellDmaWaitTagStatusAll(1u << DMA_TAG_DONE);

    sys_spu_thread_exit(0);
}
