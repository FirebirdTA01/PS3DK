/* sheap-key-mutex: the key_mutex sample's flow, written independently.
 *
 * The PPU initialises a keyed 10240-byte heap through the firmware and
 * creates no objects.  Four SPU threads race to create the key-1 mutex
 * (one creates, three wait for it and attach), each adds 1 to a PPU
 * counter 1000 times under the mutex, and the last to delete frees it.
 *
 * Prints "ans => 4000" and the SUCCEEDED line, the FAILED line, or
 * SHEAP_KEY_MUTEX_HLE (see P0).
 */
#include "harness.h"
#include "key_mutex_bin.h"

#define SPUS       4
#define INCREMENTS 1000

static uint8_t heap[10240] __attribute__((aligned(128)));
static volatile uint32_t count[4] __attribute__((aligned(16)));

int main(void)
{
    sysSpuImage image;
    uint64_t args[SPUS][4];
    int32_t status[SPUS];
    uint64_t entry;
    unsigned i;
    int ok = 0, rc;

    rc = harness_load_sheap();
    if (rc == 0)
        rc = cellKeySheapInitialize(heap, sizeof(heap), 3);
    if (rc == 0 && !harness_firmware_wrote_header(heap)) {
        printf("SHEAP_KEY_MUTEX_HLE firmware cellSheap wrote no header (built-in HLE cellSheap in use)\n");
        return 0;
    }
    if (rc == 0) {
        for (i = 0; i < SPUS; ++i) {
            args[i][0] = harness_ea(heap);
            args[i][1] = harness_ea(count);
            args[i][2] = INCREMENTS;
            args[i][3] = 0;
        }
        if (sysSpuImageImport(&image, key_mutex_bin, 0) == 0
                && harness_run(&image, SPUS, args, status) == 0) {
            sysSpuImageClose(&image);
            ok = 1;
            for (i = 0; i < SPUS; ++i)
                if (status[i] != 0) {
                    printf("spu %u check %d\n", i, (int)status[i]);
                    ok = 0;
                }
        }
    }
    printf("ans => %u\n", (unsigned)count[0]);
    memcpy(&entry, heap + 128 + 8 * 1, 8);
    ok = ok && count[0] == SPUS * INCREMENTS && entry == 0 && cellKeySheapQueryFree(heap) == 7936;
    printf("## libsheap : sample_sheap_key_mutex_ppu %s ##\n", ok ? "SUCCEEDED" : "FAILED");
    return 0;
}
