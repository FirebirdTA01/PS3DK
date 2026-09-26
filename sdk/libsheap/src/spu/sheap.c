/* sheap.c -- SPU shared heap: Initialize, Allocate, Free and the queries,
 * plus cellKeySheapInitialize.
 *
 * Every operation after initialisation takes the heap lock, works on the
 * node-state tree through the line cache, writes the modified lines back
 * and releases the lock with the header line store.
 */
#include <string.h>
#include <cell/sheap.h>
#include "sheap_spu.h"

/* LS copy of the header of the heap being operated on. */
static sheap_header header;

/* Fill the allocator part of *h for a heap at `ea` whose tree starts at
 * ea_tree and whose tree-plus-heap span is `span`, and build the tree. */
static int core_initialize(sheap_header *h, uint64_t ea, uint64_t ea_tree,
                           uint64_t span, uint32_t tag)
{
    sheap_geometry geo;

    h->lock = 0;
    h->ea_self = ea;
    h->spu_tag1 = tag;
    h->spu_tag2 = tag;
    if (__sheap_geometry(span, &geo) != 0)
        return (int)CELL_SHEAP_ERROR_INVAL;

    h->ea_tree = ea_tree;
    h->s_tree = geo.s_tree;
    h->n_nodes = geo.n_nodes;
    h->ea_heap = ea_tree + geo.s_tree;
    h->s_root = geo.s_root;
    h->s_buffer = geo.s_buffer;

    /* An all-zero tree is all FREE; then fence off the part of the root
     * block that lies past the end of the heap. */
    __sheap_dma_zero(ea_tree, geo.s_tree, tag);
    __sheap_cache_begin(ea_tree, tag);
    __sheap_tree_fence(&__sheap_cache_access, geo.n_nodes);
    __sheap_cache_flush();
    return CELL_OK;
}

int cellSheapInitialize(uint64_t ea_sheap, uint64_t size, uint32_t spu_dma_tag)
{
    int rc = __sheap_check_init_args(ea_sheap, size, spu_dma_tag);

    if (rc != CELL_OK)
        return rc;
    memset(&header, 0, sizeof(header));
    rc = core_initialize(&header, ea_sheap, ea_sheap + SHEAP_HEADER_BYTES,
                         __sheap_plain_span(size), spu_dma_tag);
    if (rc != CELL_OK)
        return rc;
    __sheap_publish(ea_sheap, &header);
    return CELL_OK;
}

int cellKeySheapInitialize(uint64_t ea_ksheap, uint64_t size, uint32_t spu_dma_tag)
{
    uint64_t ea_table = ea_ksheap + SHEAP_HEADER_BYTES;
    int rc = __sheap_check_init_args(ea_ksheap, size, spu_dma_tag);

    if (rc != CELL_OK)
        return rc;
    memset(&header, 0, sizeof(header));
    header.ea_keytable = ea_table;
    header.s_keytable = SHEAP_KEYTABLE_BYTES;
    __sheap_dma_zero(ea_table, SHEAP_KEYTABLE_BYTES, spu_dma_tag);
    rc = core_initialize(&header, ea_ksheap, ea_table + SHEAP_KEYTABLE_BYTES,
                         __sheap_keyed_span(size), spu_dma_tag);
    if (rc != CELL_OK)
        header.ea_keytable = 0;     /* marks the heap unusable for keys */
    __sheap_publish(ea_ksheap, &header);
    return rc;
}

static void begin(uint64_t ea_sheap)
{
    __sheap_lock(ea_sheap, &header);
    __sheap_cache_begin(header.ea_tree, header.spu_tag1);
}

static void end(uint64_t ea_sheap)
{
    __sheap_cache_flush();
    __sheap_unlock(ea_sheap, &header);
}

uint64_t cellSheapAllocate(uint64_t ea_sheap, uint64_t size)
{
    uint64_t block;

    if (ea_sheap == 0 || (ea_sheap & (SHEAP_HEADER_BYTES - 1)))
        return 0;
    begin(ea_sheap);
    block = __sheap_heap_allocate(&__sheap_cache_access, header.n_nodes,
                                  header.s_root, header.ea_heap, size);
    end(ea_sheap);
    return block;
}

int cellSheapFree(uint64_t ea_sheap, uint64_t ptr)
{
    int rc;

    if (ea_sheap & (SHEAP_HEADER_BYTES - 1))
        return (int)CELL_SHEAP_ERROR_ALIGN;
    if (ea_sheap == 0)
        return (int)CELL_SHEAP_ERROR_INVAL;
    begin(ea_sheap);
    rc = __sheap_heap_free(&__sheap_cache_access, header.n_nodes,
                           header.s_root, header.ea_heap, ptr);
    end(ea_sheap);
    return rc;
}

int cellSheapQueryMax(uint64_t ea_sheap)
{
    uint64_t bytes;

    if (ea_sheap & (SHEAP_HEADER_BYTES - 1))
        return (int)CELL_SHEAP_ERROR_ALIGN;
    if (ea_sheap == 0)
        return (int)CELL_SHEAP_ERROR_INVAL;
    begin(ea_sheap);
    bytes = __sheap_tree_query_max(&__sheap_cache_access, header.n_nodes,
                                   header.s_root);
    end(ea_sheap);
    return (int)bytes;
}

int cellSheapQueryFree(uint64_t ea_sheap)
{
    uint64_t bytes;

    if (ea_sheap & (SHEAP_HEADER_BYTES - 1))
        return (int)CELL_SHEAP_ERROR_ALIGN;
    if (ea_sheap == 0)
        return (int)CELL_SHEAP_ERROR_INVAL;
    begin(ea_sheap);
    bytes = __sheap_tree_query_free(&__sheap_cache_access, header.n_nodes,
                                    header.s_root);
    end(ea_sheap);
    return (int)bytes;
}
