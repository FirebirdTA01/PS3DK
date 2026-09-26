/* heap.c -- shared-heap layout arithmetic and key-table transitions.
 *
 * Layout of a heap at a 128-byte aligned address (all offsets relative):
 *
 *   plain:  [0,128) header | node-state tree | heap
 *   keyed:  [0,128) header | [128,2176) key table | node-state tree | heap
 *
 * The tree-plus-heap span is split so the tree has two bits for every node
 * of a tree sized for the whole span, rounded to 128 bytes; the heap takes
 * the rest and the root block is the heap size rounded up to a power of
 * two.  The key-table functions are single read-modify-write steps: the
 * caller reads the entry under a line reservation, calls a step, and
 * stores the result conditionally (or re-reads on WAIT).
 */
#include <cell/sheap/error.h>
#include "sheap_core.h"

uint64_t __sheap_ceil_pow2(uint64_t x)
{
    if (x <= 1)
        return x;
    if (x > ((uint64_t)1 << 63))
        return 0;
    return (uint64_t)1 << (64 - __builtin_clzll(x - 1));
}

unsigned __sheap_floor_log2(uint64_t x)
{
    return 63u - (unsigned)__builtin_clzll(x);
}

uint64_t __sheap_plain_span(uint64_t size)
{
    return (size - SHEAP_HEADER_BYTES) & ~(uint64_t)(SHEAP_LEAF_BYTES - 1);
}

uint64_t __sheap_keyed_span(uint64_t size)
{
    return (size - SHEAP_HEADER_BYTES - SHEAP_KEYTABLE_BYTES)
           & ~(uint64_t)(SHEAP_LEAF_BYTES - 1);
}

int __sheap_check_init_args(uint64_t ea, uint64_t size, uint32_t tag)
{
    if (ea == 0 || size < 0x800 || size > ((uint64_t)1 << 39) || tag > 31)
        return (int)CELL_SHEAP_ERROR_INVAL;
    if (ea & (SHEAP_HEADER_BYTES - 1))
        return (int)CELL_SHEAP_ERROR_ALIGN;
    return 0;
}

int __sheap_geometry(uint64_t tree_and_heap, sheap_geometry *geo)
{
    uint64_t estimate = tree_and_heap >> SHEAP_LEAF_SHIFT;
    uint64_t max_id, tree_bytes, leaves;

    if (estimate == 0)
        return -1;
    /* Largest node id of a tree whose leaves cover the whole span. */
    max_id = estimate + __sheap_ceil_pow2(estimate) - 1;
    if (max_id > 0xffffffffu)
        return -1;
    /* Two bits for each id 0..max_id, in whole 128-byte lines. */
    tree_bytes = ((2 * max_id + 2 + 7) >> 3);
    tree_bytes = (tree_bytes + 127) & ~(uint64_t)127;
    if (tree_bytes > tree_and_heap)
        return -1;

    geo->s_tree = tree_bytes;
    geo->s_buffer = tree_and_heap - tree_bytes;
    leaves = geo->s_buffer >> SHEAP_LEAF_SHIFT;
    geo->n_nodes = leaves ? (uint32_t)(leaves + __sheap_ceil_pow2(leaves) - 1) : 0;
    geo->s_root = geo->s_buffer ? __sheap_ceil_pow2(geo->s_buffer) : 0;
    return 0;
}

int __sheap_init_geometry(int keyed, uint64_t size, sheap_geometry *geo)
{
    uint64_t prefix = SHEAP_HEADER_BYTES + (keyed ? SHEAP_KEYTABLE_BYTES : 0);

    if (size < prefix)
        return -1;
    return __sheap_geometry(keyed ? __sheap_keyed_span(size)
                                  : __sheap_plain_span(size), geo);
}

int __sheap_size_to_row(uint64_t size, uint64_t s_root)
{
    if (size == 0)
        size = 1;
    if (size > s_root || s_root < SHEAP_LEAF_BYTES)
        return -1;
    if (size < SHEAP_LEAF_BYTES)
        size = SHEAP_LEAF_BYTES;
    return __builtin_clzll(size - 1) - __builtin_clzll(s_root - 1);
}

uint64_t __sheap_node_offset(uint32_t node, uint64_t s_root)
{
    unsigned row = __sheap_floor_log2(node);

    return (uint64_t)(node - (1u << row)) * (s_root >> row);
}

uint64_t __sheap_offset_to_leaf(uint64_t offset, uint64_t s_root)
{
    return (offset >> SHEAP_LEAF_SHIFT) + (s_root >> SHEAP_LEAF_SHIFT);
}

uint64_t __sheap_heap_allocate(const sheap_tree_access *t, uint32_t n_nodes,
                               uint64_t s_root, uint64_t ea_heap,
                               uint64_t size)
{
    int row = __sheap_size_to_row(size, s_root);
    uint32_t node;

    if (row < 0)
        return 0;
    node = __sheap_tree_allocate(t, n_nodes, (unsigned)row);
    if (node == 0)
        return 0;
    return ea_heap + __sheap_node_offset(node, s_root);
}

int __sheap_heap_free(const sheap_tree_access *t, uint32_t n_nodes,
                      uint64_t s_root, uint64_t ea_heap, uint64_t ptr)
{
    uint64_t offset, leaf;

    if (ptr < ea_heap)
        return (int)CELL_SHEAP_ERROR_INVAL;
    offset = ptr - ea_heap;
    if (offset & (SHEAP_LEAF_BYTES - 1))
        return (int)CELL_SHEAP_ERROR_INVAL;
    leaf = __sheap_offset_to_leaf(offset, s_root);
    if (leaf == 0 || leaf > n_nodes)
        return (int)CELL_SHEAP_ERROR_INVAL;
    return __sheap_tree_free(t, n_nodes, (uint32_t)leaf);
}

/* ---- key table ---------------------------------------------------- */

int __sheap_key_state(uint64_t entry)
{
    uint32_t count = SHEAP_KEY_COUNT(entry);
    uint32_t nval = SHEAP_KEY_NVAL(entry);

    if (count == 0)
        return nval == 0 ? SHEAP_KEY_EMPTY : SHEAP_KEY_DELETING;
    return nval == 0 ? SHEAP_KEY_NEWING : SHEAP_KEY_LIVE;
}

uint64_t __sheap_key_decode(uint32_t nval, uint64_t ea_heap)
{
    return ((uint64_t)(nval - 1) << SHEAP_LEAF_SHIFT) + ea_heap;
}

int __sheap_key_new_step(uint64_t entry, uint64_t ea_heap, uint64_t *next,
                         uint64_t *object)
{
    int state = __sheap_key_state(entry);
    uint32_t nval = SHEAP_KEY_NVAL(entry);

    /* Someone is between New and FinalizeNew, or between the last Delete
     * and FinalizeDelete: wait for the entry to settle. */
    if (state == SHEAP_KEY_NEWING || state == SHEAP_KEY_DELETING)
        return SHEAP_KEY_WAIT;
    *next = SHEAP_KEY_ENTRY(SHEAP_KEY_COUNT(entry) + 1, nval);
    *object = nval ? __sheap_key_decode(nval, ea_heap) : 0;
    return SHEAP_KEY_COMMIT;
}

int __sheap_key_finalize_new_step(uint64_t entry, uint64_t object,
                                  uint64_t ea_heap, uint64_t s_buffer,
                                  uint64_t *next)
{
    uint64_t nval;

    if (__sheap_key_state(entry) != SHEAP_KEY_NEWING)
        return (int)CELL_SHEAP_ERROR_BUSY;
    if (object == 0) {
        *next = 0;                      /* creation cancelled */
        return SHEAP_KEY_COMMIT;
    }
    if (object < ea_heap || object - ea_heap >= s_buffer
        || ((object - ea_heap) & (SHEAP_LEAF_BYTES - 1)))
        return (int)CELL_SHEAP_ERROR_INVAL;
    nval = ((object - ea_heap) >> SHEAP_LEAF_SHIFT) + 1;
    if (nval > 0xffffffffu)
        return SHEAP_KEY_OVERFLOW;
    *next = SHEAP_KEY_ENTRY(SHEAP_KEY_COUNT(entry), nval);
    return SHEAP_KEY_COMMIT;
}

int __sheap_key_delete_step(uint64_t entry, uint64_t ea_heap, uint64_t *next,
                            uint64_t *object)
{
    uint32_t count = SHEAP_KEY_COUNT(entry);
    uint32_t nval = SHEAP_KEY_NVAL(entry);

    if (count == 0)
        return (int)CELL_SHEAP_ERROR_BUSY;
    if (nval == 0)
        return SHEAP_KEY_WAIT;          /* still being created */
    *next = SHEAP_KEY_ENTRY(count - 1, nval);
    *object = count == 1 ? __sheap_key_decode(nval, ea_heap) : 0;
    return SHEAP_KEY_COMMIT;
}

int __sheap_key_finalize_delete_step(uint64_t entry, uint64_t *next)
{
    if (__sheap_key_state(entry) != SHEAP_KEY_DELETING)
        return (int)CELL_SHEAP_ERROR_BUSY;
    *next = 0;
    return SHEAP_KEY_COMMIT;
}
