/* keytable.c -- key-table entry updates for keyed heaps.
 *
 * An entry is the doubleword {reference count, encoded object address} at
 * ea_keytable + 8 * key; 16 entries share a 128-byte line.  Every update
 * is a read-modify-write under a reservation on that line, outside the
 * heap lock: read the line, let the portable step decide, store the line
 * conditionally, and go round again if another processor touched the line
 * in between (or if the step says to wait for a creator or deleter).
 * The transitions themselves are in core/heap.c.
 */
#include <cell/atomic.h>
#include "sheap_spu.h"

static uint64_t key_line[16] __attribute__((aligned(128)));

static uint64_t entry_ea(const sheap_header *h, CellSheapKey key)
{
    return h->ea_keytable + (uint64_t)key * sizeof(sheap_key_entry);
}

static uint64_t read_entry(uint64_t ea)
{
    uint64_t entry;

    SHEAP_DMA_BARRIER();
    entry = cellAtomicLockLine64(key_line, ea);
    SHEAP_DMA_BARRIER();
    return entry;
}

static int store_entry(uint64_t ea, uint64_t entry)
{
    int stored;

    SHEAP_DMA_BARRIER();
    stored = cellAtomicStoreConditional64(key_line, ea, entry);
    SHEAP_DMA_BARRIER();
    return stored;
}

uint64_t __sheap_key_new_object(const sheap_header *h, CellSheapKey key)
{
    uint64_t ea = entry_ea(h, key), next, object;

    for (;;) {
        if (__sheap_key_new_step(read_entry(ea), h->ea_heap, &next, &object)
            != SHEAP_KEY_COMMIT)
            continue;
        if (store_entry(ea, next))
            return object;
    }
}

int __sheap_key_finalize_new(const sheap_header *h, CellSheapKey key,
                             uint64_t object)
{
    uint64_t ea = entry_ea(h, key), next;

    for (;;) {
        int rc = __sheap_key_finalize_new_step(read_entry(ea), object,
                                               h->ea_heap, h->s_buffer, &next);

        if (rc == SHEAP_KEY_OVERFLOW)
            __builtin_trap();       /* address cannot be encoded */
        if (rc != SHEAP_KEY_COMMIT)
            return rc;
        if (store_entry(ea, next))
            return CELL_OK;
    }
}

int __sheap_key_delete_object(const sheap_header *h, CellSheapKey key,
                              uint64_t *object)
{
    uint64_t ea = entry_ea(h, key), next;

    for (;;) {
        int rc = __sheap_key_delete_step(read_entry(ea), h->ea_heap, &next,
                                         object);

        if (rc == SHEAP_KEY_WAIT)
            continue;
        if (rc != SHEAP_KEY_COMMIT)
            return rc;
        if (store_entry(ea, next))
            return CELL_OK;
    }
}

int __sheap_key_finalize_delete(const sheap_header *h, CellSheapKey key)
{
    uint64_t ea = entry_ea(h, key), next;

    for (;;) {
        int rc = __sheap_key_finalize_delete_step(read_entry(ea), &next);

        if (rc != SHEAP_KEY_COMMIT)
            return rc;
        if (store_entry(ea, next))
            return CELL_OK;
    }
}
