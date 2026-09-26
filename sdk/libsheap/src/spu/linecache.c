/* linecache.c -- write-back LS cache of node-state tree lines.
 *
 * The tree lives in main memory (two bits per node, 512 nodes per 128-byte
 * line) and can be far larger than LS, so operations touch it through a
 * small fully associative cache of whole lines.  Lines are fetched with a
 * plain GET and written back with a plain PUT on the heap's DMA tag; this
 * is only valid while the heap lock is held, so every locked operation
 * starts with __sheap_cache_begin and ends with __sheap_cache_flush before
 * the lock is released.  A path from a leaf to the root touches at most
 * one line per 9 rows, so the cache rarely evicts during Allocate/Free.
 */
#include <spu_mfcio.h>
#include "sheap_spu.h"

#define WAYS 16u

static uint32_t cache_data[WAYS][32] __attribute__((aligned(128)));
static uint64_t cache_line[WAYS];        /* line index within the tree */
static uint8_t cache_valid[WAYS];
static uint8_t cache_dirty[WAYS];
static unsigned cache_victim;
static unsigned cache_last;
static uint64_t cache_tree;
static unsigned cache_tag;

void __sheap_cache_begin(uint64_t ea_tree, unsigned tag)
{
    unsigned way;

    for (way = 0; way < WAYS; ++way)
        cache_valid[way] = cache_dirty[way] = 0;
    cache_victim = cache_last = 0;
    cache_tree = ea_tree;
    cache_tag = tag;
}

static void write_back(unsigned way)
{
    SHEAP_DMA_BARRIER();
    mfc_put(cache_data[way], cache_tree + cache_line[way] * 128, 128, cache_tag, 0, 0);
    cache_dirty[way] = 0;
}

void __sheap_cache_flush(void)
{
    unsigned way, any = 0;

    for (way = 0; way < WAYS; ++way)
        if (cache_valid[way] && cache_dirty[way]) {
            write_back(way);
            any = 1;
        }
    if (any)
        __sheap_dma_wait(cache_tag);
}

/* The way holding the line for `node`, loading it on a miss. */
static unsigned way_for(uint32_t node)
{
    uint64_t line = SHEAP_TREE_LINE(node);
    unsigned way;

    if (cache_valid[cache_last] && cache_line[cache_last] == line)
        return cache_last;
    for (way = 0; way < WAYS; ++way)
        if (cache_valid[way] && cache_line[way] == line)
            return cache_last = way;

    way = cache_victim;
    cache_victim = (cache_victim + 1) % WAYS;
    if (cache_valid[way] && cache_dirty[way]) {
        write_back(way);
        __sheap_dma_wait(cache_tag);
    }
    cache_line[way] = line;
    cache_valid[way] = 1;
    SHEAP_DMA_BARRIER();
    mfc_get(cache_data[way], cache_tree + line * 128, 128, cache_tag, 0, 0);
    __sheap_dma_wait(cache_tag);
    return cache_last = way;
}

static unsigned cache_get(void *ctx, uint32_t node)
{
    unsigned way = way_for(node);

    (void)ctx;
    return (cache_data[way][(node >> 4) & 31u] >> SHEAP_TREE_SHIFT(node)) & 3u;
}

static void cache_set(void *ctx, uint32_t node, unsigned state)
{
    unsigned way = way_for(node);
    uint32_t *word = &cache_data[way][(node >> 4) & 31u];
    unsigned shift = SHEAP_TREE_SHIFT(node);

    (void)ctx;
    *word = (*word & ~(3u << shift)) | ((uint32_t)state << shift);
    cache_dirty[way] = 1;
}

const sheap_tree_access __sheap_cache_access = { cache_get, cache_set, 0 };
