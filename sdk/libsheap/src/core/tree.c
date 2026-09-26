/* tree.c -- node-state tree algorithms for the shared heap.
 *
 * See sheap_core.h for node numbering and states.  Two behaviours are kept
 * on purpose because the firmware has them and heaps are shared with it,
 * so both sides must hand out the same addresses:
 *
 *   - a row scan stops one short of the row's last candidate id, so the
 *     rightmost node of every row is never allocated (for the leaf row
 *     that is the last real leaf, and the root is never allocated);
 *   - Free climbs from the pointer's leaf to the first non-FREE node and
 *     frees it without checking that it was an allocation.
 */
#include <cell/sheap/error.h>
#include "sheap_core.h"

#define GET(t, n)       ((t)->get((t)->ctx, (n)))
#define SET(t, n, s)    ((t)->set((t)->ctx, (n), (s)))

static int taken(unsigned state)
{
    return state == SHEAP_NODE_USED || state == SHEAP_NODE_FULL;
}

/* Record that `node` is now allocated and restate its ancestors: parents
 * whose two children are both taken become FULL, and the rest of the path
 * up to the first node already SPLIT becomes SPLIT. */
static void mark_used(const sheap_tree_access *t, uint32_t node)
{
    uint32_t parent;

    SET(t, node, SHEAP_NODE_USED);
    while (node > 1 && taken(GET(t, node ^ 1u))) {
        node >>= 1;
        SET(t, node, SHEAP_NODE_FULL);
    }
    for (parent = node >> 1; parent != 0; parent >>= 1) {
        if (GET(t, parent) == SHEAP_NODE_SPLIT)
            break;
        SET(t, parent, SHEAP_NODE_SPLIT);
    }
}

void __sheap_tree_fence(const sheap_tree_access *t, uint32_t n_nodes)
{
    /* Walk up the right edge of the real tree.  Wherever the rightmost
     * real node of a row is a left child, its right sibling covers only
     * memory past the end of the heap: mark that sibling allocated. */
    uint32_t n = n_nodes;

    while (n > 1 && __sheap_ceil_pow2(n) - n != 1) {
        if ((n & 1u) == 0)
            mark_used(t, n + 1);
        n >>= 1;
    }
}

/* The highest taken node on the path from `node` to the root, or 0. */
static uint32_t highest_taken(const sheap_tree_access *t, uint32_t node)
{
    uint32_t top = 0;

    for (; node != 0; node >>= 1)
        if (taken(GET(t, node)))
            top = node;
    return top;
}

uint32_t __sheap_tree_allocate(const sheap_tree_access *t, uint32_t n_nodes,
                               unsigned row)
{
    uint64_t first, end, i;

    if (row > 31)
        return 0;
    first = (uint64_t)1 << row;
    end = (first << 1) - 1;
    if (end > n_nodes)
        end = n_nodes;

    i = first;
    while (i < end) {
        uint32_t node = (uint32_t)i;
        uint32_t top = 0;
        uint64_t next;

        if (GET(t, node) == SHEAP_NODE_FREE) {
            uint32_t up = node >> 1;
            unsigned state = SHEAP_NODE_FREE;

            while (up != 0) {
                state = GET(t, up);
                if (state != SHEAP_NODE_FREE)
                    break;
                up >>= 1;
            }
            if (up == 0 || state == SHEAP_NODE_SPLIT) {
                mark_used(t, node);
                return node;
            }
            /* Inside a block that is already allocated: climb to the
             * outermost taken ancestor. */
            top = up;
            for (up >>= 1; up != 0 && taken(GET(t, up)); up >>= 1)
                top = up;
        } else {
            top = highest_taken(t, node);
            if (top == 0) {
                ++i;
                continue;
            }
        }
        /* Resume at the first node of this row to the right of `top`'s
         * subtree. */
        next = (uint64_t)top + 1;
        while (next <= i)
            next <<= 1;
        i = next;
    }
    return 0;
}

int __sheap_tree_free(const sheap_tree_access *t, uint32_t n_nodes,
                      uint32_t leaf)
{
    uint32_t target, node;

    if (leaf == 0 || leaf > n_nodes)
        return (int)CELL_SHEAP_ERROR_INVAL;
    if (GET(t, leaf) == SHEAP_NODE_SPLIT)
        return (int)CELL_SHEAP_ERROR_FAULT;

    target = leaf;
    while (GET(t, target) == SHEAP_NODE_FREE) {
        target >>= 1;
        if (target == 0)
            return (int)CELL_SHEAP_ERROR_INVAL;
    }
    SET(t, target, SHEAP_NODE_FREE);

    /* Full ancestors now have room again. */
    for (node = target >> 1; node != 0 && GET(t, node) == SHEAP_NODE_FULL;
         node >>= 1)
        SET(t, node, SHEAP_NODE_SPLIT);

    /* Merge free buddies upward. */
    node = target;
    while (node != 1 && GET(t, node ^ 1u) == SHEAP_NODE_FREE) {
        node >>= 1;
        SET(t, node, SHEAP_NODE_FREE);
    }
    return 0;
}

/* Visit, in pre-order, every node reachable from the root through SPLIT
 * nodes, i.e. every node that is not inside an allocated block.  A FREE
 * node reached this way is a maximal free block; the FREE subtree left
 * under an allocated (USED) node is never reached.  Returns the free
 * bytes. */
static uint64_t walk_free(const sheap_tree_access *t, uint32_t n_nodes,
                          uint64_t s_root)
{
    uint64_t total = 0;
    uint64_t n = 1;

    if (n_nodes == 0)
        return 0;
    for (;;) {
        unsigned state = GET(t, (uint32_t)n);

        if (state == SHEAP_NODE_SPLIT && (n << 1) <= n_nodes) {
            n <<= 1;
            continue;
        }
        if (state == SHEAP_NODE_FREE)
            total += s_root >> __sheap_floor_log2(n);
        /* Next node: the right sibling of the nearest left child on the
         * path back up. */
        while (n & 1u)
            n >>= 1;
        if (n == 0)
            break;
        n += 1;
    }
    return total;
}

uint64_t __sheap_tree_query_max(const sheap_tree_access *t, uint32_t n_nodes,
                                uint64_t s_root)
{
    /* Firmware rule, kept for parity with heaps the PPU also queries: the
     * block size of the first FREE id in 1..n_nodes, inclusive, else 0.
     * Unlike Allocate it scans every row, includes each row's rightmost
     * id, and does not look at ancestors, so a FREE node inside an
     * allocated block counts.  The result is therefore an upper bound on
     * the largest block Allocate can return, not that block's size. */
    uint64_t n;

    for (n = 1; n <= n_nodes; ++n)
        if (GET(t, (uint32_t)n) == SHEAP_NODE_FREE)
            return s_root >> __sheap_floor_log2(n);
    return 0;
}

uint64_t __sheap_tree_query_free(const sheap_tree_access *t, uint32_t n_nodes,
                                 uint64_t s_root)
{
    return walk_free(t, n_nodes, s_root);
}
