/* sheap-p2-shared: a heap initialised by the PPU firmware and used from
 * both sides, proving they share one lock and one allocation tree.
 *
 *   PPU  cellSheapInitialize(10240)                    (firmware)
 *   SPU  A = Allocate(512) at +256, B = Allocate(512) at +768
 *   PPU  C = Allocate(512) at +1280                     (firmware)
 *   SPU  Free(A), D = Allocate(128) at +256, queries
 *   PPU  queries equal the SPU's; Free B, C, D          (firmware)
 *   SPU  queries back to the empty heap
 *
 * Prints SHEAP_P2_OK, SHEAP_P2_FAIL <detail>, or SHEAP_P2_HLE (see P0).
 */
#include "harness.h"
#include "p2_bin.h"

static uint8_t heap[10240] __attribute__((aligned(128)));
static harness_result result;

static int run_phase(sysSpuImage *image, uint64_t phase, uint64_t to_free)
{
    uint64_t args[1][4] = { { harness_ea(heap), phase, to_free, harness_ea(&result) } };
    int32_t status[1];

    memset(&result, 0, sizeof(result));
    if (harness_run(image, 1, args, status) != 0) {
        printf("SHEAP_P2_FAIL spu thread setup (phase %u)\n", (unsigned)phase);
        return -1;
    }
    if (status[0] != 0) {
        printf("SHEAP_P2_FAIL spu phase %u check %d (values +%lld +%lld)\n", (unsigned)phase,
               (int)status[0], (long long)(result.values[0] - harness_ea(heap)),
               (long long)(result.values[1] - harness_ea(heap)));
        return -1;
    }
    return 0;
}

int main(void)
{
    sysSpuImage image;
    uint8_t *a, *b, *c, *d;
    int rc;

    rc = harness_load_sheap();
    if (rc != 0) {
        printf("SHEAP_P2_FAIL load sheap module rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    memset(heap, 0xee, sizeof(heap));
    rc = cellSheapInitialize(heap, sizeof(heap), 8);
    if (rc != 0) {
        printf("SHEAP_P2_FAIL cellSheapInitialize rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    if (!harness_firmware_wrote_header(heap)) {
        printf("SHEAP_P2_HLE firmware cellSheap wrote no header (built-in HLE cellSheap in use)\n");
        return 0;
    }
    if (sysSpuImageImport(&image, p2_bin, 0) != 0) {
        printf("SHEAP_P2_FAIL image import\n");
        return 0;
    }

    if (run_phase(&image, 1, 0) != 0)
        return 0;
    a = (uint8_t *)(uintptr_t)result.values[0];
    b = (uint8_t *)(uintptr_t)result.values[1];
    if (a != heap + 256 || b != heap + 768) {
        printf("SHEAP_P2_FAIL SPU blocks +%ld/+%ld (want +256/+768)\n",
               (long)(a - heap), (long)(b - heap));
        return 0;
    }
    c = cellSheapAllocate(heap, 512);
    if (c != heap + 1280) {
        printf("SHEAP_P2_FAIL PPU allocate after SPU: +%ld (want +1280)\n", (long)(c - heap));
        return 0;
    }
    if (run_phase(&image, 2, harness_ea(a)) != 0)
        return 0;
    d = (uint8_t *)(uintptr_t)result.values[0];
    if (d != heap + 256) {
        printf("SHEAP_P2_FAIL SPU block D +%ld (want +256)\n", (long)(d - heap));
        return 0;
    }
    if ((int)result.values[2] != 8832 || (int)result.values[3] != 4096
            || cellSheapQueryFree(heap) != 8832 || cellSheapQueryMax(heap) != 4096) {
        printf("SHEAP_P2_FAIL queries: SPU %d/%d, PPU %d/%d (want 8832/4096)\n",
               (int)result.values[2], (int)result.values[3],
               cellSheapQueryFree(heap), cellSheapQueryMax(heap));
        return 0;
    }
    if (cellSheapFree(heap, b) != 0 || cellSheapFree(heap, c) != 0 || cellSheapFree(heap, d) != 0) {
        printf("SHEAP_P2_FAIL PPU free of SPU/PPU blocks\n");
        return 0;
    }
    if (run_phase(&image, 3, 0) != 0)
        return 0;
    sysSpuImageClose(&image);
    if ((int)result.values[2] != 9984 || (int)result.values[3] != 8192) {
        printf("SHEAP_P2_FAIL SPU sees %d/%d after PPU frees (want 9984/8192)\n",
               (int)result.values[2], (int)result.values[3]);
        return 0;
    }
    printf("SHEAP_P2_OK PPU and SPU share the heap (A/B/C/D at +256/+768/+1280/+256)\n");
    return 0;
}
