/* sheap_core.h -- portable shared-heap algorithms.
 *
 * Everything here is plain integer arithmetic over a node-state accessor,
 * with no DMA, locking or byte order, so the same sources build for the SPU
 * archive and for the host unit tests (tests/sdk/sheap-core-host-test.sh).
 *
 * The heap is a binary buddy tree.  Nodes are numbered from 1 like an
 * implicit binary heap: node n has children 2n and 2n+1, lies in row
 * floor(log2 n), and covers the block of s_root >> row bytes that starts
 * (n - 2^row) blocks into the heap.  Leaves are 128-byte blocks.  Node ids
 * run up to n_nodes, the id of the last leaf that is backed by real heap
 * memory; everything to its right is fenced off at initialisation.
 */
#ifndef PS3DK_SHEAP_CORE_H
#define PS3DK_SHEAP_CORE_H

#include <stdint.h>

/* Two-bit node states, as stored in the shared node-state tree. */
enum {
    SHEAP_NODE_FREE = 0,    /* node and whole subtree free */
    SHEAP_NODE_USED = 1,    /* allocated as one block; subtree ignored */
    SHEAP_NODE_SPLIT = 2,   /* something below is used, some space remains */
    SHEAP_NODE_FULL = 3     /* split and nothing below is free */
};

#define SHEAP_LEAF_SHIFT      7u
#define SHEAP_LEAF_BYTES      128u
#define SHEAP_HEADER_BYTES    128u
#define SHEAP_KEYTABLE_BYTES  2048u     /* 256 entries of 8 bytes */

/* Node-state storage: node n lives in 32-bit word n/16 of the tree, two
 * bits wide, first node of the word in the most significant bits. */
#define SHEAP_TREE_WORD(n)    ((uint64_t)(n) >> 4)
#define SHEAP_TREE_SHIFT(n)   (30u - 2u * ((uint32_t)(n) & 15u))
#define SHEAP_TREE_LINE(n)    ((uint64_t)(n) >> 9)      /* 512 nodes per 128 B */

typedef struct sheap_tree_access {
    unsigned (*get)(void *ctx, uint32_t node);
    void (*set)(void *ctx, uint32_t node, unsigned state);
    void *ctx;
} sheap_tree_access;

/* Sizes derived from the tree-plus-heap byte count at initialisation. */
typedef struct sheap_geometry {
    uint64_t s_tree;      /* node-state tree bytes (multiple of 128) */
    uint64_t s_buffer;    /* usable heap bytes */
    uint64_t s_root;      /* root block bytes (power of two, or 0) */
    uint32_t n_nodes;     /* largest valid node id */
} sheap_geometry;

uint64_t __sheap_ceil_pow2(uint64_t x);
unsigned __sheap_floor_log2(uint64_t x);

/* 0 on success, -1 when the sizes do not describe a usable tree. */
int __sheap_geometry(uint64_t tree_and_heap, sheap_geometry *geo);

/* Tree-plus-heap byte count for a plain or keyed heap of `size` bytes.
 * Arithmetic wraps like the firmware's; a wrapped value fails geometry. */
uint64_t __sheap_plain_span(uint64_t size);
uint64_t __sheap_keyed_span(uint64_t size);

/* Argument checks shared by both Initialize functions: 0, or a
 * CELL_SHEAP_ERROR_* code. */
int __sheap_check_init_args(uint64_t ea, uint64_t size, uint32_t tag);

/* Tree operations.  The tree must already read as all FREE for fence. */
void __sheap_tree_fence(const sheap_tree_access *t, uint32_t n_nodes);
uint32_t __sheap_tree_allocate(const sheap_tree_access *t, uint32_t n_nodes,
                               unsigned row);
int __sheap_tree_free(const sheap_tree_access *t, uint32_t n_nodes,
                      uint32_t leaf);
uint64_t __sheap_tree_query_max(const sheap_tree_access *t, uint32_t n_nodes,
                                uint64_t s_root);
uint64_t __sheap_tree_query_free(const sheap_tree_access *t, uint32_t n_nodes,
                                 uint64_t s_root);

/* Heap arithmetic.  size_to_row returns -1 when no block can hold size. */
int __sheap_size_to_row(uint64_t size, uint64_t s_root);
uint64_t __sheap_node_offset(uint32_t node, uint64_t s_root);
/* Leaf node holding a heap offset (the caller checks range and alignment). */
uint64_t __sheap_offset_to_leaf(uint64_t offset, uint64_t s_root);

/* Whole Allocate and Free on an already locked heap.  Allocate returns the
 * block's address or 0; Free returns 0 or a CELL_SHEAP_ERROR_* code. */
uint64_t __sheap_heap_allocate(const sheap_tree_access *t, uint32_t n_nodes,
                               uint64_t s_root, uint64_t ea_heap,
                               uint64_t size);
int __sheap_heap_free(const sheap_tree_access *t, uint32_t n_nodes,
                      uint64_t s_root, uint64_t ea_heap, uint64_t ptr);

/* Key-table entries.  An entry is one big-endian doubleword: the reference
 * count in the high word and the encoded object address in the low word
 * (0 = nothing published, else ((ea - ea_heap) >> 7) + 1). */
enum {
    SHEAP_KEY_EMPTY,      /* (0, 0) */
    SHEAP_KEY_NEWING,     /* (c, 0): the creator is building the object */
    SHEAP_KEY_LIVE,       /* (c, v): published, c references */
    SHEAP_KEY_DELETING    /* (0, v): last reference gone, being freed */
};

/* Results of one step of a key-table read-modify-write. */
#define SHEAP_KEY_COMMIT      0    /* store *next conditionally */
#define SHEAP_KEY_WAIT        1    /* re-read the entry and try again */
#define SHEAP_KEY_OVERFLOW    2    /* encoded address does not fit */

#define SHEAP_KEY_ENTRY(count, nval) \
    (((uint64_t)(uint32_t)(count) << 32) | (uint32_t)(nval))
#define SHEAP_KEY_COUNT(e)    ((uint32_t)((e) >> 32))
#define SHEAP_KEY_NVAL(e)     ((uint32_t)(e))

int __sheap_key_state(uint64_t entry);
uint64_t __sheap_key_decode(uint32_t nval, uint64_t ea_heap);

/* NewObject: take a reference.  On COMMIT, *object is 0 for the creator
 * or the published object's address. */
int __sheap_key_new_step(uint64_t entry, uint64_t ea_heap, uint64_t *next,
                         uint64_t *object);
/* FinalizeNew: publish `object`, or cancel creation when it is 0.
 * Returns COMMIT, OVERFLOW or a CELL_SHEAP_ERROR_* code. */
int __sheap_key_finalize_new_step(uint64_t entry, uint64_t object,
                                  uint64_t ea_heap, uint64_t s_buffer,
                                  uint64_t *next);
/* DeleteObject: drop a reference.  On COMMIT, *object is the address to
 * free when this was the last reference, else 0.  Returns COMMIT, WAIT or
 * CELL_SHEAP_ERROR_BUSY. */
int __sheap_key_delete_step(uint64_t entry, uint64_t ea_heap, uint64_t *next,
                            uint64_t *object);
/* FinalizeDelete: empty a DELETING entry.  COMMIT or CELL_SHEAP_ERROR_BUSY. */
int __sheap_key_finalize_delete_step(uint64_t entry, uint64_t *next);

#endif /* PS3DK_SHEAP_CORE_H */
