/*
 * cell/sync/queue_types.h -- CellSyncQueue descriptor layout.
 *
 * A 32-byte EA-resident descriptor for a bounded SPU-side FIFO.
 * The head word packs rlock / index / wlock / size into a single
 * 64-bit value manipulated by MFC getllar/putllc.
 *
 * Field widths (independent from Cell BE Handbook Ch. 19):
 *   rlock  (8 bits)  -- read-side lock during DMA transfer
 *   index  (24 bits) -- monotonic write pointer; slot = index % depth
 *   wlock  (8 bits)  -- write-side lock during DMA transfer
 *   size   (24 bits) -- current element count
 */
#ifndef _PS3DK_CELL_SYNC_QUEUE_TYPES_H_SPU_
#define _PS3DK_CELL_SYNC_QUEUE_TYPES_H_SPU_

#include <stdint.h>

/* Descriptor size and alignment. */
#define CELL_SYNC_QUEUE_DESCRIPTOR_SIZE  32
#define CELL_SYNC_QUEUE_ALIGN            128

/* Maximum element size routed through the 128-byte LS DMA staging buffer. */
#define CELL_SYNC_QUEUE_MAX_ELEMENT_SIZE 128

/* --- Packed head-word accessors (no C bit-fields) --- */

static inline uint8_t  _cellSyncQueueGetRlock(uint64_t head) {
    return (uint8_t)(head >> 56);
}
static inline uint32_t _cellSyncQueueGetIndex(uint64_t head) {
    return (uint32_t)((head >> 32) & 0xFFFFFFu);
}
static inline uint8_t  _cellSyncQueueGetWlock(uint64_t head) {
    return (uint8_t)((head >> 24) & 0xFFu);
}
static inline uint32_t _cellSyncQueueGetSize(uint64_t head) {
    return (uint32_t)(head & 0xFFFFFFu);
}

static inline uint64_t _cellSyncQueueMakeHead(uint8_t rlock, uint32_t index,
                                               uint8_t wlock, uint32_t size) {
    return ((uint64_t)rlock << 56)
         | ((uint64_t)(index & 0xFFFFFFu) << 32)
         | ((uint64_t)wlock << 24)
         | ((uint64_t)(size  & 0xFFFFFFu));
}

/* --- EA-resident descriptor (32 bytes, 128-byte aligned at ea_queue) --- */

typedef struct {
    uint64_t head;          /* [ 0.. 7] packed rlock|index|wlock|size */
    uint32_t buffer_size;   /* [ 8..11] bytes per element */
    uint32_t depth;         /* [12..15] element capacity */
    uint64_t buffer_ea;     /* [16..23] EA of the data ring buffer */
    uint32_t tag;           /* [24..27] DMA tag for element transfers */
    uint32_t _pad;          /* [28..31] pad to 32 bytes */
} CellSyncQueue;

#endif /* _PS3DK_CELL_SYNC_QUEUE_TYPES_H_SPU_ */
