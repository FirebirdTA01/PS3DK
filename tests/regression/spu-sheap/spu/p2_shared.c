/* sheap-p2-shared (SPU side): one phase of a heap the PPU initialised
 * through the firmware and both sides use.
 *
 * args: heap EA, phase, a block to free (phase 2), result block EA.
 *   phase 1: allocate two 512-byte blocks        -> values[0..1]
 *   phase 2: free the first, allocate 128 bytes  -> values[0] = block,
 *            values[1] = QueryFree, values[2] = QueryMax
 *   phase 3: after the PPU freed everything      -> values[1..2] queries
 */
#include "common.h"

int main(uint64_t heap, uint64_t phase, uint64_t to_free, uint64_t ea_result)
{
    if (phase == 1) {
        row_result[0] = cellSheapAllocate(heap, 512);
        row_result[1] = cellSheapAllocate(heap, 512);
        CHECK(1, row_result[0] == heap + 256);
        CHECK(2, row_result[1] == heap + 768);
    } else if (phase == 2) {
        CHECK(3, cellSheapFree(heap, to_free) == CELL_OK);
        row_result[0] = cellSheapAllocate(heap, 128);
        CHECK(4, row_result[0] == heap + 256);
    }
    row_result[1] = (uint64_t)(int64_t)cellSheapQueryFree(heap);
    row_result[2] = (uint64_t)(int64_t)cellSheapQueryMax(heap);
    row_finish(ea_result);
    return 0;
}
