/* sheap-p1-spu (SPU side): an SPU-only heap.  The SPU initialises a plain
 * 20480-byte heap in PPU memory and runs a fixed allocate/free/query
 * script.  Expected offsets and sizes come from the portable core run on
 * the host (tests/sdk/sheap-core-host-test.sh builds the same code).
 *
 * args: heap EA, result block EA.
 * result: values[0] = number of script steps passed.
 */
#include "common.h"

static unsigned steps;

#define STEP(n, cond) do { CHECK(n, cond); if (!row_failed) ++steps; } while (0)

int main(uint64_t heap, uint64_t ea_result, uint64_t unused3, uint64_t unused4)
{
    uint64_t a, b, c, d, e, f;

    (void)unused3;
    (void)unused4;

    STEP(1, cellSheapInitialize(heap, 20480, 5) == CELL_OK);
    STEP(2, cellSheapQueryFree(heap) == 20224 && cellSheapQueryMax(heap) == 16384);
    a = cellSheapAllocate(heap, 2048);
    b = cellSheapAllocate(heap, 100);
    c = cellSheapAllocate(heap, 0);
    d = cellSheapAllocate(heap, 8192);
    e = cellSheapAllocate(heap, 16384);
    STEP(3, a == heap + 256);
    STEP(4, b == heap + 2304);
    STEP(5, c == heap + 2432);          /* size 0 takes a 128-byte block */
    STEP(6, d == heap + 8448);
    STEP(7, e == 0);                    /* no 16 KB node is allocatable */
    STEP(8, cellSheapQueryFree(heap) == 9728 && cellSheapQueryMax(heap) == 4096);
    STEP(9, cellSheapFree(heap, b) == CELL_OK);
    STEP(10, cellSheapFree(heap, a + 64) == (int)CELL_SHEAP_ERROR_INVAL);   /* misaligned */
    STEP(11, cellSheapFree(heap, heap + 128) == (int)CELL_SHEAP_ERROR_INVAL); /* below the heap */
    STEP(12, cellSheapFree(heap + 64, a) == (int)CELL_SHEAP_ERROR_ALIGN);
    STEP(13, cellSheapQueryFree(heap) == 9856);
    STEP(14, cellSheapFree(heap, a) == CELL_OK && cellSheapQueryFree(heap) == 11904);
    f = cellSheapAllocate(heap, 4096);
    STEP(15, f == heap + 4352 && cellSheapQueryFree(heap) == 7808);
    STEP(16, cellSheapFree(heap, c) == CELL_OK && cellSheapFree(heap, d) == CELL_OK
             && cellSheapFree(heap, f) == CELL_OK);
    STEP(17, cellSheapQueryFree(heap) == 20224 && cellSheapQueryMax(heap) == 16384);
    STEP(18, cellSheapInitialize(heap, 2047, 5) == (int)CELL_SHEAP_ERROR_INVAL);

    row_result[0] = steps;
    row_finish(ea_result);
    return 0;
}
