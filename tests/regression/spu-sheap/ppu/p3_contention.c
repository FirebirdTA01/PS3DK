/* sheap-p3-contention: keyed objects under contention, with a mutex the
 * PPU and six SPUs take in turn.
 *
 * The PPU initialises a keyed 10240-byte heap and creates the key-5 mutex
 * through the firmware, then starts six SPU threads (spu/p3_contention.c).
 * Each SPU announces itself in a shared READY word and waits for START.
 * Once all six are ready the PPU sets START and takes the same mutex
 * itself, at least ROUNDS times, reading the mutex counter inside each of
 * its critical sections.  SPU increments seen between the PPU's first and
 * last acquisitions prove that SPU and PPU acquisitions interleaved; the
 * PPU keeps taking the mutex (up to a bound) until it has seen one, so the
 * witness does not depend on scheduling luck.
 *
 * Afterwards: every SPU's result block must report ROUNDS completed rounds
 * and that it saw START (a result block that never arrived fails the
 * row), both counters must be exact, every key the SPUs used must be empty
 * again, and the heap must be back to its initial free size once the PPU
 * drops its mutex reference.
 *
 * Prints SHEAP_P3_OK, SHEAP_P3_FAIL <detail>, or SHEAP_P3_HLE (see P0).
 */
#include "harness.h"
#include "p3_bin.h"

#define SPUS       6
#define ROUNDS     200
#define MAX_ROUNDS (ROUNDS * 1000)          /* bound on the PPU's extra rounds */
#define READY_SPIN 400000000u               /* bound on waiting for the SPUs */

/* One word per 128-byte line (word offsets match spu/p3_contention.c). */
#define MUTEX_COUNTER     0
#define SEMAPHORE_COUNTER 32
#define READY             64
#define START             96

static uint8_t heap[10240] __attribute__((aligned(128)));
static volatile uint32_t shared[128] __attribute__((aligned(128)));
static harness_result results[SPUS];

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
    uint32_t ppu_rounds, spu_seen_first = 0, spu_seen_last = 0;
    unsigned i, spin;
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

    /* Fill the result blocks so a block that never arrives cannot pass. */
    memset(results, 0xff, sizeof(results));
    for (i = 0; i < SPUS; ++i) {
        args[i][0] = harness_ea(heap);
        args[i][1] = harness_ea(shared);
        args[i][2] = ROUNDS;
        args[i][3] = harness_ea(&results[i]);
    }
    if (sysSpuImageImport(&image, p3_bin, 0) != 0 || harness_start(&group, &image, SPUS, args) != 0) {
        printf("SHEAP_P3_FAIL spu thread setup\n");
        return 0;
    }

    /* Start line: every SPU is running and waiting before the PPU starts. */
    for (spin = 0; shared[READY] != SPUS && spin < READY_SPIN; ++spin)
        ;
    if (shared[READY] != SPUS) {
        printf("SHEAP_P3_FAIL only %u of %u SPUs reached the start line\n",
               (unsigned)shared[READY], SPUS);
        return 0;
    }
    __asm__ __volatile__("sync" ::: "memory");
    shared[START] = 1;

    for (ppu_rounds = 0; ppu_rounds < ROUNDS
                         || (spu_seen_last == spu_seen_first && ppu_rounds < MAX_ROUNDS);
         ++ppu_rounds) {
        uint32_t value;

        if (cellKeySheapMutexLock(&mutex) != 0) {
            printf("SHEAP_P3_FAIL PPU lock\n");
            return 0;
        }
        /* SPU increments so far = counter minus the PPU's own. */
        value = shared[MUTEX_COUNTER];
        if (ppu_rounds == 0)
            spu_seen_first = value;
        spu_seen_last = value - ppu_rounds;
        shared[MUTEX_COUNTER] = value + 1;
        cellKeySheapMutexUnlock(&mutex);
    }

    if (harness_join(&group, status) != 0) {
        printf("SHEAP_P3_FAIL spu join\n");
        return 0;
    }
    sysSpuImageClose(&image);
    __asm__ __volatile__("sync" ::: "memory");   /* SPU DMA results are in memory */
    for (i = 0; i < SPUS; ++i) {
        if (status[i] != 0) {
            printf("SHEAP_P3_FAIL spu %u check %d\n", i, (int)status[i]);
            return 0;
        }
        if (results[i].values[0] != ROUNDS || results[i].values[1] != 1) {
            printf("SHEAP_P3_FAIL spu %u result block: rounds 0x%llx (want %u), start seen 0x%llx\n",
                   i, (unsigned long long)results[i].values[0], ROUNDS,
                   (unsigned long long)results[i].values[1]);
            return 0;
        }
    }
    if (spu_seen_last == spu_seen_first) {
        printf("SHEAP_P3_FAIL no SPU acquisition between the PPU's first and last "
               "(%u PPU rounds)\n", (unsigned)ppu_rounds);
        return 0;
    }
    if (shared[MUTEX_COUNTER] != SPUS * ROUNDS + ppu_rounds
            || shared[SEMAPHORE_COUNTER] != SPUS * ROUNDS) {
        printf("SHEAP_P3_FAIL counters mutex %u (want %u) semaphore %u (want %u)\n",
               (unsigned)shared[MUTEX_COUNTER], (unsigned)(SPUS * ROUNDS + ppu_rounds),
               (unsigned)shared[SEMAPHORE_COUNTER], SPUS * ROUNDS);
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
    printf("SHEAP_P3_OK %u SPUs x %u rounds + PPU x %u on one mutex, counters exact, "
           "%u SPU acquisitions between the PPU's first and last, result blocks and keys checked\n",
           SPUS, ROUNDS, (unsigned)ppu_rounds, (unsigned)(spu_seen_last - spu_seen_first));
    return 0;
}
