/* sheap-p0-layout: the firmware's shared-heap layout as the SPU library
 * reads it.
 *
 * The PPU initialises a plain and a keyed 10240-byte heap through the
 * cellSheap firmware stub.  An SPU program reads both headers, the first
 * tree lines and the key table and compares them with the layout libsheap
 * assumes.  Then the PPU allocates through the stub and checks the
 * addresses the SPU library would also hand out.
 *
 * Prints SHEAP_P0_OK, SHEAP_P0_FAIL <detail>, or SHEAP_P0_HLE when the
 * firmware call returned success without writing the heap (the emulator's
 * built-in cellSheap is in use instead of the firmware module), which is
 * a configuration problem, not a pass.
 */
#include "harness.h"
#include "p0_bin.h"

static uint8_t plain[10240] __attribute__((aligned(128)));
static uint8_t keyed[10240] __attribute__((aligned(128)));
static harness_result result;

int main(void)
{
    sysSpuImage image;
    uint64_t args[1][4];
    int32_t status[1];
    void *a, *b;
    int rc;
    unsigned i;

    rc = harness_load_sheap();
    if (rc != 0) {
        printf("SHEAP_P0_FAIL load sheap module rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    memset(plain, 0xee, sizeof(plain));
    memset(keyed, 0xee, sizeof(keyed));
    rc = cellSheapInitialize(plain, sizeof(plain), 8);
    if (rc != 0) {
        printf("SHEAP_P0_FAIL cellSheapInitialize rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    rc = cellKeySheapInitialize(keyed, sizeof(keyed), 3);
    if (rc != 0) {
        printf("SHEAP_P0_FAIL cellKeySheapInitialize rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    if (!harness_firmware_wrote_header(plain) || !harness_firmware_wrote_header(keyed)) {
        printf("SHEAP_P0_HLE firmware cellSheap returned success but wrote no header: "
               "the built-in (HLE) cellSheap is in use, not the firmware module\n");
        return 0;
    }

    args[0][0] = harness_ea(plain);
    args[0][1] = harness_ea(keyed);
    args[0][2] = harness_ea(&result);
    args[0][3] = 0;
    if (sysSpuImageImport(&image, p0_bin, 0) != 0 || harness_run(&image, 1, args, status) != 0) {
        printf("SHEAP_P0_FAIL spu thread setup\n");
        return 0;
    }
    sysSpuImageClose(&image);
    if (status[0] != 0) {
        printf("SHEAP_P0_FAIL layout: %u mismatch(es), first check %d",
               (unsigned)result.values[0], (int)status[0]);
        for (i = 0; i < result.values[0] && i < 7; ++i)
            printf(" [check %u = 0x%llx]", (unsigned)result.values[1 + 2 * i],
                   (unsigned long long)result.values[2 + 2 * i]);
        printf("\n");
        return 0;
    }

    /* Addresses the firmware hands out match the SPU library's rules. */
    a = cellSheapAllocate(plain, 2048);
    b = cellKeySheapAllocate(keyed, 128);
    if (a != plain + 256 || b != keyed + 2304) {
        printf("SHEAP_P0_FAIL firmware allocate: plain +%ld (want +256), keyed +%ld (want +2304)\n",
               (long)((uint8_t *)a - plain), (long)((uint8_t *)b - keyed));
        return 0;
    }
    if (cellSheapQueryFree(plain) != 9984 - 2048 || cellKeySheapQueryFree(keyed) != 7936 - 128) {
        printf("SHEAP_P0_FAIL firmware QueryFree %d / %d (want %d / %d)\n",
               cellSheapQueryFree(plain), cellKeySheapQueryFree(keyed), 9984 - 2048, 7936 - 128);
        return 0;
    }
    /* The first-FREE-node QueryMax rule libsheap follows: plain -> node 5
     * (4096), keyed -> node 5 (2048). */
    if (cellSheapQueryMax(plain) != 4096 || cellKeySheapQueryMax(keyed) != 2048) {
        printf("SHEAP_P0_FAIL firmware QueryMax %d / %d (libsheap rule gives 4096 / 2048)\n",
               cellSheapQueryMax(plain), cellKeySheapQueryMax(keyed));
        return 0;
    }
    printf("SHEAP_P0_OK firmware layout matches (plain + keyed headers, tree lines, key table, "
           "allocation addresses, QueryFree, QueryMax rule)\n");
    return 0;
}
