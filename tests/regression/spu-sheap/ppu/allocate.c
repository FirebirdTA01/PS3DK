/* sheap-allocate: the allocate sample's flow, written independently.
 *
 * The PPU initialises a 10240-byte heap through the firmware; one SPU
 * allocates a 2 KB work area from it, fills it with the first 512 primes
 * and returns the last one.
 *
 * Prints "512 th PRIME NUMBER is 3671" and the SUCCEEDED line on success,
 * the FAILED line otherwise, or SHEAP_ALLOCATE_HLE (see P0).
 */
#include "harness.h"
#include "allocate_bin.h"

#define COUNT 512

static uint8_t heap[10240] __attribute__((aligned(128)));
static volatile uint32_t answer[4] __attribute__((aligned(16)));

int main(void)
{
    sysSpuImage image;
    uint64_t args[1][4];
    int32_t status[1] = { -1 };
    int ok = 0;

    if (harness_load_sheap() == 0 && cellSheapInitialize(heap, sizeof(heap), 8) == 0) {
        if (!harness_firmware_wrote_header(heap)) {
            printf("SHEAP_ALLOCATE_HLE firmware cellSheap wrote no header (built-in HLE cellSheap in use)\n");
            return 0;
        }
        args[0][0] = harness_ea(heap);
        args[0][1] = COUNT;
        args[0][2] = harness_ea(answer);
        args[0][3] = 0;
        if (sysSpuImageImport(&image, allocate_bin, 0) == 0
                && harness_run(&image, 1, args, status) == 0) {
            sysSpuImageClose(&image);
            printf("%d th PRIME NUMBER is %u\n\n", COUNT, (unsigned)answer[0]);
            ok = status[0] == 0 && answer[0] == 3671 && cellSheapQueryFree(heap) == 9984;
        }
    }
    printf("## libsheap : sample_sheap_allocate_ppu %s ##\n", ok ? "SUCCEEDED" : "FAILED");
    if (!ok)
        printf("(spu status %d)\n", (int)status[0]);
    printf("Exit PPU\n");
    return 0;
}
