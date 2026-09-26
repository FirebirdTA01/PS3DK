/* sheap_layout.h -- the shared heap's 128-byte header line.
 *
 * The header is written by whichever side initialises the heap (the PPU
 * firmware or an SPU) and read by every Allocate, Free and Query, so its
 * layout is fixed.  All fields are big-endian in memory; the SPU reads them
 * natively.  Word 0 is the heap lock.  Fields after `lock` never change
 * once the heap is initialised; the keyed fields are 0 on a plain heap.
 *
 * Kept free of SPU headers so the host unit test can check the offsets.
 */
#ifndef PS3DK_SHEAP_LAYOUT_H
#define PS3DK_SHEAP_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct sheap_header {
    uint32_t lock;            /* 0 free, 1 held */
    uint32_t reserved0;
    uint64_t ea_self;         /* the heap's own address */
    uint64_t ea_tree;         /* node-state tree */
    uint64_t s_tree;          /* tree bytes */
    uint32_t n_nodes;         /* largest valid node id */
    uint32_t reserved1;
    uint64_t ea_heap;         /* first heap byte */
    uint64_t s_root;          /* root block bytes */
    uint64_t s_buffer;        /* usable heap bytes */
    uint32_t spu_tag1;        /* DMA tag for all SPU heap traffic */
    uint32_t spu_tag2;
    uint64_t ea_keytable;     /* keyed heaps: key table, else 0 */
    uint64_t s_keytable;      /* keyed heaps: 2048 */
    uint8_t  reserved2[40];
} __attribute__((aligned(128))) sheap_header;

#define SHEAP_LAYOUT_CHECK(name, cond) \
    typedef char sheap_layout_check_##name[(cond) ? 1 : -1] __attribute__((unused))

SHEAP_LAYOUT_CHECK(lock, offsetof(sheap_header, lock) == 0);
SHEAP_LAYOUT_CHECK(ea_self, offsetof(sheap_header, ea_self) == 8);
SHEAP_LAYOUT_CHECK(ea_tree, offsetof(sheap_header, ea_tree) == 16);
SHEAP_LAYOUT_CHECK(s_tree, offsetof(sheap_header, s_tree) == 24);
SHEAP_LAYOUT_CHECK(n_nodes, offsetof(sheap_header, n_nodes) == 32);
SHEAP_LAYOUT_CHECK(ea_heap, offsetof(sheap_header, ea_heap) == 40);
SHEAP_LAYOUT_CHECK(s_root, offsetof(sheap_header, s_root) == 48);
SHEAP_LAYOUT_CHECK(s_buffer, offsetof(sheap_header, s_buffer) == 56);
SHEAP_LAYOUT_CHECK(spu_tag1, offsetof(sheap_header, spu_tag1) == 64);
SHEAP_LAYOUT_CHECK(spu_tag2, offsetof(sheap_header, spu_tag2) == 68);
SHEAP_LAYOUT_CHECK(ea_keytable, offsetof(sheap_header, ea_keytable) == 72);
SHEAP_LAYOUT_CHECK(s_keytable, offsetof(sheap_header, s_keytable) == 80);
SHEAP_LAYOUT_CHECK(size, sizeof(sheap_header) == 128);

/* Key-table entry: {count, encoded address}, one doubleword; 16 per line. */
typedef struct sheap_key_entry {
    uint32_t count;
    uint32_t nval;
} sheap_key_entry;

SHEAP_LAYOUT_CHECK(entry_count, offsetof(sheap_key_entry, count) == 0);
SHEAP_LAYOUT_CHECK(entry_nval, offsetof(sheap_key_entry, nval) == 4);
SHEAP_LAYOUT_CHECK(entry_size, sizeof(sheap_key_entry) == 8);

#undef SHEAP_LAYOUT_CHECK

#endif /* PS3DK_SHEAP_LAYOUT_H */
