/* cell/spurs/queue.h - SPURS FIFO queue primitive.
 *
 * CellSpursQueue is a 128-byte opaque block plus a caller-provided
 * backing buffer (size * depth bytes).  The SPRX manages head / tail
 * indices; push / pop may block or return ERROR_BUSY depending on
 * whether the blocking variant is called.
 */
#ifndef __PS3DK_CELL_SPURS_QUEUE_H__
#define __PS3DK_CELL_SPURS_QUEUE_H__

#include <stdint.h>
#include <stdbool.h>
#include <cell/spurs/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CellSpursQueueDirection {
    CELL_SPURS_QUEUE_SPU2SPU = 0,
    CELL_SPURS_QUEUE_SPU2PPU = 1,
    CELL_SPURS_QUEUE_PPU2SPU = 2
} CellSpursQueueDirection;

#define CELL_SPURS_QUEUE_ALIGN  128
#define CELL_SPURS_QUEUE_SIZE   128

typedef struct CellSpursQueue {
    unsigned char skip[CELL_SPURS_QUEUE_SIZE];
} __attribute__((aligned(CELL_SPURS_QUEUE_ALIGN))) CellSpursQueue;

#ifdef __SPU__

/* SPU side: the queue lives in main memory and is named by its
 * effective address.  An entry moves between the caller's LS buffer and
 * the queue by DMA on `tag`: *Begin starts the transfer and *End
 * completes it.  Blocking forms are valid only in a SPURS task.
 * Declared only: the SPU runtime does not implement these yet. */
extern int _cellSpursQueueInitialize(uint64_t ea, uint64_t buffer,
                                     unsigned int size, unsigned int depth,
                                     CellSpursQueueDirection direction,
                                     unsigned isIwl);
extern int _cellSpursQueuePushBegin(uint64_t ea, const void *buffer,
                                    unsigned int tag, unsigned isBlocking);
extern int cellSpursQueuePushEnd(uint64_t ea, unsigned int tag);
extern int _cellSpursQueuePopBegin(uint64_t ea, void *buffer,
                                   unsigned int tag, unsigned isBlocking);
extern int _cellSpursQueuePopEnd(uint64_t ea, unsigned int tag,
                                 unsigned isPeek);
extern int cellSpursQueueSize(uint64_t ea, unsigned int *size);
extern int cellSpursQueueDepth(uint64_t ea, unsigned int *depth);
extern int cellSpursQueueClear(uint64_t ea);
extern int cellSpursQueueGetDirection(uint64_t ea,
                                      CellSpursQueueDirection *direction);
extern int cellSpursQueueGetEntrySize(uint64_t ea, unsigned int *entry_size);
extern int cellSpursQueueGetTasksetAddress(uint64_t ea, uint64_t *pEaTaskset);

#define cellSpursQueueInitialize(ea, buffer, size, depth, direction) \
    _cellSpursQueueInitialize((ea), (buffer), (size), (depth), (direction), 0)
#define cellSpursQueueInitializeIWL(ea, buffer, size, depth, direction) \
    _cellSpursQueueInitialize((ea), (buffer), (size), (depth), (direction), 1)
#define cellSpursQueuePushBegin(ea, buffer, tag) \
    _cellSpursQueuePushBegin((ea), (buffer), (tag), 1)
#define cellSpursQueueTryPushBegin(ea, buffer, tag) \
    _cellSpursQueuePushBegin((ea), (buffer), (tag), 0)
#define cellSpursQueuePopBegin(ea, buffer, tag) \
    _cellSpursQueuePopBegin((ea), (buffer), (tag), 1)
#define cellSpursQueueTryPopBegin(ea, buffer, tag) \
    _cellSpursQueuePopBegin((ea), (buffer), (tag), 0)
#define cellSpursQueuePeekBegin(ea, buffer, tag) \
    _cellSpursQueuePopBegin((ea), (buffer), (tag), 1)
#define cellSpursQueueTryPeekBegin(ea, buffer, tag) \
    _cellSpursQueuePopBegin((ea), (buffer), (tag), 0)
#define cellSpursQueuePopEnd(ea, tag)  _cellSpursQueuePopEnd((ea), (tag), 0)
#define cellSpursQueuePeekEnd(ea, tag) _cellSpursQueuePopEnd((ea), (tag), 1)

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class Queue : public CellSpursQueue {
public:
    static const uint32_t kAlign = CELL_SPURS_QUEUE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_QUEUE_SIZE;

    static const CellSpursQueueDirection kSpu2Spu = CELL_SPURS_QUEUE_SPU2SPU;
    static const CellSpursQueueDirection kSpu2Ppu = CELL_SPURS_QUEUE_SPU2PPU;
    static const CellSpursQueueDirection kPpu2Spu = CELL_SPURS_QUEUE_PPU2SPU;
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif /* __cplusplus */

#else /* PPU */

extern int _cellSpursQueueInitialize(CellSpurs *spurs,
                                     CellSpursTaskset *taskset,
                                     CellSpursQueue *queue,
                                     const void *buffer,
                                     unsigned int size,
                                     unsigned int depth,
                                     CellSpursQueueDirection direction);

extern int cellSpursQueueAttachLv2EventQueue(CellSpursQueue *queue);
extern int cellSpursQueueDetachLv2EventQueue(CellSpursQueue *queue);

extern int cellSpursQueuePushBody(CellSpursQueue *queue, const void *buffer, bool isBlocking);
extern int cellSpursQueuePopBody(CellSpursQueue *queue, void *buffer, bool isPeek, bool isBlocking);

extern int cellSpursQueueSize(CellSpursQueue *queue, unsigned int *size);
extern int cellSpursQueueDepth(CellSpursQueue *queue, unsigned int *depth);
extern int cellSpursQueueClear(CellSpursQueue *queue);
extern int cellSpursQueueGetDirection(CellSpursQueue *queue, CellSpursQueueDirection *direction);
extern int cellSpursQueueGetEntrySize(CellSpursQueue *queue, unsigned int *entry_size);
extern int cellSpursQueueGetTasksetAddress(const CellSpursQueue *queue,
                                           CellSpursTaskset **taskset);

static inline int cellSpursQueueTryPush(CellSpursQueue *q, const void *buf)
{ return cellSpursQueuePushBody(q, buf, false); }

static inline int cellSpursQueuePush(CellSpursQueue *q, const void *buf)
{ return cellSpursQueuePushBody(q, buf, true); }

static inline int cellSpursQueueTryPop(CellSpursQueue *q, void *buf)
{ return cellSpursQueuePopBody(q, buf, false, false); }

static inline int cellSpursQueuePop(CellSpursQueue *q, void *buf)
{ return cellSpursQueuePopBody(q, buf, false, true); }

static inline int cellSpursQueueTryPeek(CellSpursQueue *q, void *buf)
{ return cellSpursQueuePopBody(q, buf, true, false); }

static inline int cellSpursQueuePeek(CellSpursQueue *q, void *buf)
{ return cellSpursQueuePopBody(q, buf, true, true); }

static inline int
cellSpursQueueInitialize(CellSpursTaskset *taskset,
                         CellSpursQueue *queue,
                         const void *buffer,
                         unsigned int size,
                         unsigned int depth,
                         CellSpursQueueDirection direction)
{ return _cellSpursQueueInitialize(0, taskset, queue, buffer, size, depth, direction); }

static inline int
cellSpursQueueInitializeIWL(CellSpurs *spurs,
                            CellSpursQueue *queue,
                            const void *buffer,
                            unsigned int size,
                            unsigned int depth,
                            CellSpursQueueDirection direction)
{ return _cellSpursQueueInitialize(spurs, 0, queue, buffer, size, depth, direction); }

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class Queue : public CellSpursQueue {
public:
    static const uint32_t kAlign = CELL_SPURS_QUEUE_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_QUEUE_SIZE;

    static const CellSpursQueueDirection kSpu2Spu = CELL_SPURS_QUEUE_SPU2SPU;
    static const CellSpursQueueDirection kSpu2Ppu = CELL_SPURS_QUEUE_SPU2PPU;
    static const CellSpursQueueDirection kPpu2Spu = CELL_SPURS_QUEUE_PPU2SPU;

    static int initialize(CellSpursTaskset *taskset,
                          CellSpursQueue *queue,
                          const void *buffer,
                          unsigned int size,
                          unsigned int depth,
                          CellSpursQueueDirection direction)
    { return cellSpursQueueInitialize(taskset, queue, buffer, size, depth, direction); }

    static int initializeIWL(CellSpurs *spurs,
                             CellSpursQueue *queue,
                             const void *buffer,
                             unsigned int size,
                             unsigned int depth,
                             CellSpursQueueDirection direction)
    { return cellSpursQueueInitializeIWL(spurs, queue, buffer, size, depth, direction); }

    int push(const void *buffer)
    { return cellSpursQueuePush(this, buffer); }

    int tryPush(const void *buffer)
    { return cellSpursQueueTryPush(this, buffer); }

    int pushBody(const void *buffer, bool isBlocking)
    { return cellSpursQueuePushBody(this, buffer, isBlocking); }

    int pop(void *buffer)
    { return cellSpursQueuePop(this, buffer); }

    int tryPop(void *buffer)
    { return cellSpursQueueTryPop(this, buffer); }

    int popBody(void *buffer, bool isPeek, bool isBlocking)
    { return cellSpursQueuePopBody(this, buffer, isPeek, isBlocking); }

    int tryPeek(void *buffer)
    { return cellSpursQueueTryPeek(this, buffer); }

    int peek(void *buffer)
    { return cellSpursQueuePeek(this, buffer); }

    int attachLv2EventQueue()
    { return cellSpursQueueAttachLv2EventQueue(this); }

    int detachLv2EventQueue()
    { return cellSpursQueueDetachLv2EventQueue(this); }

    int attach()
    { return cellSpursQueueAttachLv2EventQueue(this); }

    int detach()
    { return cellSpursQueueDetachLv2EventQueue(this); }

    int size(unsigned int *value)
    { return cellSpursQueueSize(this, value); }

    int depth(unsigned int *value)
    { return cellSpursQueueDepth(this, value); }

    int clear()
    { return cellSpursQueueClear(this); }

    int getDirection(CellSpursQueueDirection *direction)
    { return cellSpursQueueGetDirection(this, direction); }

    int getEntrySize(unsigned int *entrySize)
    { return cellSpursQueueGetEntrySize(this, entrySize); }

    int getTasksetAddress(CellSpursTaskset **taskset) const
    { return cellSpursQueueGetTasksetAddress(this, taskset); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_QUEUE_H__ */
