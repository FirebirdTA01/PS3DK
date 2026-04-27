/*
 * hello-ppu-spurs-queue - PPU-visible SPURS queue bring-up.
 *
 * Creates a 4-SPU Spurs2, initializes a 4-entry SPU2PPU queue with
 * 16-byte entries (in-workload variant - no taskset needed), then
 * reads back the size / depth / direction through the Get accessors.
 * No actual queueing - the SPU side is needed to push real entries,
 * but this test confirms the queue-init path and read-back
 * surface reach the SPRX correctly.
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <malloc.h>

#include <sys/process.h>
#include <sys/spu_thread_group.h>
#include <cell/spurs.h>
#include <spu_printf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define QUEUE_ENTRY_SIZE  16
#define QUEUE_DEPTH       4
#define QUEUE_ALIGN       16

int main(void)
{
    std::printf("hello-ppu-spurs-queue\n");

    int rc = spu_printf_initialize(999, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::SpursAttribute attr;
    cell::Spurs::SpursAttribute::initialize(&attr, 4, 100, 2, false);
    attr.setNamePrefix("Queue", 5);
    attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    attr.enableSpuPrintfIfAvailable();

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    if (rc) { std::printf("  Spurs2::initialize: %#x\n", rc); return 1; }
    std::printf("  Spurs2 up\n");

    /* Backing buffer: depth * entry_size, aligned to the queue's
     * required alignment (128-byte line for cache coherency). */
    void *backing = memalign(128, QUEUE_ENTRY_SIZE * QUEUE_DEPTH);
    if (!backing) { std::printf("  memalign failed\n"); goto teardown; }

    CellSpursQueue q;
    std::memset(&q, 0, sizeof(q));

    rc = cellSpursQueueInitializeIWL(spurs, &q, backing,
                                     QUEUE_ENTRY_SIZE, QUEUE_DEPTH,
                                     CELL_SPURS_QUEUE_SPU2PPU);
    if (rc) { std::printf("  QueueInitializeIWL: %#x\n", rc); goto free_buf; }
    std::printf("  Queue init ok (entry=%d depth=%d SPU2PPU)\n",
                QUEUE_ENTRY_SIZE, QUEUE_DEPTH);

    {
        unsigned int sz = 0, dp = 0, es = 0;
        CellSpursQueueDirection dir;
        if (cellSpursQueueSize(&q, &sz) == 0)
            std::printf("  Size=%u\n", sz);
        if (cellSpursQueueDepth(&q, &dp) == 0)
            std::printf("  Depth=%u\n", dp);
        if (cellSpursQueueGetEntrySize(&q, &es) == 0)
            std::printf("  EntrySize=%u\n", es);
        if (cellSpursQueueGetDirection(&q, &dir) == 0)
            std::printf("  Direction=%d\n", (int)dir);
    }

    /* Try-pop on an empty queue should return ERROR_BUSY. */
    {
        uint8_t buf[QUEUE_ENTRY_SIZE];
        rc = cellSpursQueueTryPop(&q, buf);
        std::printf("  TryPop (expect busy): %#x\n", rc);
    }

free_buf:
    free(backing);

teardown:
    rc = spurs->finalize();
    std::printf("  Spurs finalize: %#x\n", rc);
    delete spurs;
    spu_printf_finalize();

    std::printf("hello-ppu-spurs-queue: done\n");
    return 0;
}
