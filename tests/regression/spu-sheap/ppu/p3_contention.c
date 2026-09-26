/* sheap-p3-contention: keyed objects under contention, with a mutex the
 * PPU and six SPUs take in turn.
 *
 * The PPU initialises a keyed 10240-byte heap and creates the key-5 mutex
 * through the firmware, then runs six SPU threads (see spu/p3_contention.c)
 * and meanwhile takes the same mutex ROUNDS times itself.  Afterwards both
 * counters must be exact, every key the SPUs used must be empty again,
 * and the heap must be back to its initial free size once the PPU drops
 * its mutex reference.
 *
 * Prints SHEAP_P3_OK, SHEAP_P3_FAIL <detail>, or SHEAP_P3_HLE (see P0).
 */
#include "harness.h"
#include "p3_bin.h"

#define SPUS   6
#define ROUNDS 200

static uint8_t heap[10240] __attribute__((aligned(128)));
static volatile uint32_t counters[64] __attribute__((aligned(128)));  /* [0] mutex, [32] semaphore */
static harness_result result;

static uint64_t key_entry(unsigned key)
{
    uint64_t e;

    memcpy(&e, heap + 128 + 8 * key, 8);
    return e;
}

int main(void)
{
    sysSpuImage image;
    harness_group group;
    CellKeySheapMutex mutex;
    uint64_t args[SPUS][4];
    int32_t status[SPUS];
    unsigned i;
    int rc;

    rc = harness_load_sheap();
    if (rc == 0) {
        rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYNC);
        if (rc == (int)CELL_SYSMODULE_LOADED)
            rc = 0;
    }
    if (rc != 0) {
        printf("SHEAP_P3_FAIL load modules rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    memset(heap, 0xee, sizeof(heap));
    rc = cellKeySheapInitialize(heap, sizeof(heap), 3);
    if (rc != 0) {
        printf("SHEAP_P3_FAIL cellKeySheapInitialize rc=0x%08x\n", (unsigned)rc);
        return 0;
    }
    if (!harness_firmware_wrote_header(heap)) {
        printf("SHEAP_P3_HLE firmware cellSheap wrote no header (built-in HLE cellSheap in use)\n");
        return 0;
    }
    rc = cellKeySheapMutexNew(&mutex, heap, 5);
    if (rc != 0) {
        printf("SHEAP_P3_FAIL PPU cellKeySheapMutexNew rc=0x%08x\n", (unsigned)rc);
        return 0;
    }

    for (i = 0; i < SPUS; ++i) {
        args[i][0] = harness_ea(heap);
        args[i][1] = harness_ea(counters);
        args[i][2] = ROUNDS;
        args[i][3] = i == 0 ? harness_ea(&result) : 0;
    }
    if (sysSpuImageImport(&image, p3_bin, 0) != 0 || harness_start(&group, &image, SPUS, args) != 0) {
        printf("SHEAP_P3_FAIL spu thread setup\n");
        return 0;
    }
    for (i = 0; i < ROUNDS; ++i) {
        if (cellKeySheapMutexLock(&mutex) != 0) {
            printf("SHEAP_P3_FAIL PPU lock\n");
            return 0;
        }
        counters[0] = counters[0] + 1;
        cellKeySheapMutexUnlock(&mutex);
    }
    if (harness_join(&group, status) != 0) {
        printf("SHEAP_P3_FAIL spu join\n");
        return 0;
    }
    sysSpuImageClose(&image);
    for (i = 0; i < SPUS; ++i)
        if (status[i] != 0) {
            printf("SHEAP_P3_FAIL spu %u check %d\n", i, (int)status[i]);
            return 0;
        }
    if (counters[0] != (SPUS + 1) * ROUNDS || counters[32] != SPUS * ROUNDS) {
        printf("SHEAP_P3_FAIL counters mutex %u (want %u) semaphore %u (want %u)\n",
               (unsigned)counters[0], (SPUS + 1) * ROUNDS, (unsigned)counters[32], SPUS * ROUNDS);
        return 0;
    }
    if (key_entry(6) != 0 || key_entry(7) != 0 || (key_entry(5) >> 32) != 1) {
        printf("SHEAP_P3_FAIL key entries 5/6/7 = %016llx %016llx %016llx\n",
               (unsigned long long)key_entry(5), (unsigned long long)key_entry(6),
               (unsigned long long)key_entry(7));
        return 0;
    }
    cellKeySheapMutexDelete(&mutex);
    if (key_entry(5) != 0 || cellKeySheapQueryFree(heap) != 7936) {
        printf("SHEAP_P3_FAIL after the last delete: entry 5 %016llx, QueryFree %d (want 7936)\n",
               (unsigned long long)key_entry(5), cellKeySheapQueryFree(heap));
        return 0;
    }
    printf("SHEAP_P3_OK %u SPUs + PPU, %u mutex rounds each, counters %u/%u exact, keys empty\n",
           SPUS, ROUNDS, (unsigned)counters[0], (unsigned)counters[32]);
    return 0;
}
