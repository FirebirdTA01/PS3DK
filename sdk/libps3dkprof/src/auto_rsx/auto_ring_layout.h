#ifndef PS3DK_PROF_AUTO_RING_LAYOUT_H
#define PS3DK_PROF_AUTO_RING_LAYOUT_H
#include <stdint.h>

#define auto_ring_layout ps3dk_prof__auto_ring_layout

/* Attach-time partition and budgets of the owner report ring. Defaults: 16 MiB
 * of MAIN extents and 16,384 metadata records; label 245 (PROFILER block, not
 * used by the profiler samples). The local pool is split as 16 clock reports,
 * then the legacy pairs, then blocks x 126 ring pairs at the top. 126 pairs is
 * the hardware-tested maximum copy; one block occupies a whole 4096-byte MAIN
 * extent (126 x 32 = 4032 bytes). */
enum { AUTO_RING_DEFAULT_BYTES=16u<<20, AUTO_RING_DEFAULT_RECORDS=16384u, AUTO_RING_DEFAULT_LABEL=245u,
       AUTO_RING_PAGE=1u<<20, AUTO_RING_BYTES_MAX=256u<<20, AUTO_RING_RECORDS_MAX=65536u,
       AUTO_RING_STRIDE=4096u, AUTO_RING_CLOCK_REPORTS=16u, AUTO_RING_BLOCK_PAIRS=126u };

typedef struct auto_ring_plan {
    uint32_t blocks, pairs, first, legacy_pairs;  /* first: the first ring report; ring reports end the range */
    uint32_t bytes, stride, slots;                /* MAIN extents: bytes/stride descriptors, all IO mapped */
    uint32_t records, metadata_stride, slot_stride;
    /* One LV2 allocation of whole 1 MiB pages: [0, bytes) is the IO-mapped MAIN
     * ring; CPU-only metadata entries at metadata_at and descriptors at slots_at
     * follow it, never mapped. total is the allocation's size. */
    uint32_t metadata_at, slots_at, total;
} auto_ring_plan;

/* Pure: no allocation or device access. 0 with *plan zeroed for any refusal:
 * blocks outside 2..4, a report range outside 18..1024 even reports within
 * 0..2047, an odd first ring report, an empty legacy pool, ring bytes not a
 * whole number of 1 MiB pages or above 256 MiB, records above 65,536, or a
 * total above UINT32_MAX. bytes/records 0 select the defaults. The entry sizes
 * are the caller's sizeof values. */
int auto_ring_layout(uint32_t report_first, uint32_t report_count, uint32_t blocks, uint32_t bytes,
                     uint32_t records, uint32_t metadata_stride, uint32_t slot_stride, auto_ring_plan *plan);
#endif
