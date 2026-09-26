/* cell/spurs/event_flag.h - SPURS event-flag sync primitive.
 *
 * Independent header.  CellSpursEventFlag is a 128-byte opaque block
 * (SPU local-storage-resident, referenced from PPU via DMA) that the
 * libspurs SPRX interprets.  Initialize with either the taskset or
 * in-workload (IWL) variant; set / clear / wait bits from PPU or SPU
 * depending on the direction flag set at init.
 *
 * Wait modes:
 *   OR  - wake when any bit in `bits` is set
 *   AND - wake when every bit in `bits` is set
 *
 * Clear modes:
 *   AUTO   - cellSpursEventFlagWait atomically clears the bits it saw
 *   MANUAL - caller clears bits explicitly via cellSpursEventFlagClear
 *
 * Direction decides which side (PPU or SPU) may call the
 * set/clear/wait entry points.  ANY2ANY is the least restrictive.
 */
#ifndef __PS3DK_CELL_SPURS_EVENT_FLAG_H__
#define __PS3DK_CELL_SPURS_EVENT_FLAG_H__

#include <stdint.h>
#include <cell/spurs/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CellSpursEventFlagWaitMode {
    CELL_SPURS_EVENT_FLAG_OR  = 0,
    CELL_SPURS_EVENT_FLAG_AND = 1
} CellSpursEventFlagWaitMode;

typedef enum CellSpursEventFlagClearMode {
    CELL_SPURS_EVENT_FLAG_CLEAR_AUTO   = 0,
    CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL = 1
} CellSpursEventFlagClearMode;

typedef enum CellSpursEventFlagDirection {
    CELL_SPURS_EVENT_FLAG_SPU2SPU = 0,
    CELL_SPURS_EVENT_FLAG_SPU2PPU = 1,
    CELL_SPURS_EVENT_FLAG_PPU2SPU = 2,
    CELL_SPURS_EVENT_FLAG_ANY2ANY = 3
} CellSpursEventFlagDirection;

#define CELL_SPURS_EVENT_FLAG_ALIGN  128
#define CELL_SPURS_EVENT_FLAG_SIZE   128

typedef struct CellSpursEventFlag {
    unsigned char skip[CELL_SPURS_EVENT_FLAG_SIZE];
} __attribute__((aligned(CELL_SPURS_EVENT_FLAG_ALIGN))) CellSpursEventFlag;

#ifdef __SPU__

/* SPU side: the event flag lives in main memory and is named by its
 * effective address.  Wait blocks and is valid only in a SPURS task.
 * Declared only: the SPU runtime (libspurs.a) does not implement these
 * yet, so callers compile and fail at link time. */
int _cellSpursEventFlagInitialize(uint64_t ea,
                                  CellSpursEventFlagClearMode clearMode,
                                  CellSpursEventFlagDirection direction,
                                  unsigned isIwl);
int cellSpursEventFlagSet(uint64_t ea, uint16_t bits);
int cellSpursEventFlagClear(uint64_t ea, uint16_t bits);
int _cellSpursEventFlagWait(uint64_t ea, uint16_t *bits,
                            CellSpursEventFlagWaitMode mode,
                            unsigned isBlocking);
int cellSpursEventFlagGetDirection(uint64_t ea,
                                   CellSpursEventFlagDirection *direction);
int cellSpursEventFlagGetClearMode(uint64_t ea,
                                   CellSpursEventFlagClearMode *clear_mode);
int cellSpursEventFlagGetTasksetAddress(uint64_t ea, uint64_t *pEaTaskset);

#define cellSpursEventFlagInitialize(ea, mode, direction) \
    _cellSpursEventFlagInitialize((ea), (mode), (direction), 0)
#define cellSpursEventFlagInitializeIWL(ea, mode, direction) \
    _cellSpursEventFlagInitialize((ea), (mode), (direction), 1)
#define cellSpursEventFlagWait(ea, bits, mode) \
    _cellSpursEventFlagWait((ea), (bits), (mode), 1)
#define cellSpursEventFlagTryWait(ea, bits, mode) \
    _cellSpursEventFlagWait((ea), (bits), (mode), 0)

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class EventFlag : public CellSpursEventFlag {
public:
    static const uint32_t kAlign = CELL_SPURS_EVENT_FLAG_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_EVENT_FLAG_SIZE;

    static const CellSpursEventFlagWaitMode  kOr          = CELL_SPURS_EVENT_FLAG_OR;
    static const CellSpursEventFlagWaitMode  kAnd         = CELL_SPURS_EVENT_FLAG_AND;
    static const CellSpursEventFlagClearMode kClearAuto   = CELL_SPURS_EVENT_FLAG_CLEAR_AUTO;
    static const CellSpursEventFlagClearMode kClearManual = CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL;
    static const CellSpursEventFlagDirection kSpu2Spu     = CELL_SPURS_EVENT_FLAG_SPU2SPU;
    static const CellSpursEventFlagDirection kSpu2Ppu     = CELL_SPURS_EVENT_FLAG_SPU2PPU;
    static const CellSpursEventFlagDirection kPpu2Spu     = CELL_SPURS_EVENT_FLAG_PPU2SPU;
    static const CellSpursEventFlagDirection kAny2Any     = CELL_SPURS_EVENT_FLAG_ANY2ANY;
};

/* SPU handle on an event flag in main memory: holds its EA and
 * forwards to the EA-based C API above. */
class EventFlagStub {
protected:
    uint64_t object_ea;

public:
    static const uint32_t kAlign = CELL_SPURS_EVENT_FLAG_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_EVENT_FLAG_SIZE;

    static const CellSpursEventFlagWaitMode  kOr          = CELL_SPURS_EVENT_FLAG_OR;
    static const CellSpursEventFlagWaitMode  kAnd         = CELL_SPURS_EVENT_FLAG_AND;
    static const CellSpursEventFlagClearMode kClearAuto   = CELL_SPURS_EVENT_FLAG_CLEAR_AUTO;
    static const CellSpursEventFlagClearMode kClearManual = CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL;
    static const CellSpursEventFlagDirection kSpu2Spu     = CELL_SPURS_EVENT_FLAG_SPU2SPU;
    static const CellSpursEventFlagDirection kSpu2Ppu     = CELL_SPURS_EVENT_FLAG_SPU2PPU;
    static const CellSpursEventFlagDirection kPpu2Spu     = CELL_SPURS_EVENT_FLAG_PPU2SPU;
    static const CellSpursEventFlagDirection kAny2Any     = CELL_SPURS_EVENT_FLAG_ANY2ANY;

    void setObject(uint64_t ea) { object_ea = ea; }
    uint64_t getObject(void) const { return object_ea; }

    int initialize(CellSpursEventFlagClearMode clearMode,
                   CellSpursEventFlagDirection direction) const
    { return cellSpursEventFlagInitialize(object_ea, clearMode, direction); }
    int initializeIWL(CellSpursEventFlagClearMode clearMode,
                      CellSpursEventFlagDirection direction) const
    { return cellSpursEventFlagInitializeIWL(object_ea, clearMode, direction); }

    int set(uint16_t bits) const
    { return cellSpursEventFlagSet(object_ea, bits); }
    int clear(uint16_t bits) const
    { return cellSpursEventFlagClear(object_ea, bits); }
    int wait(uint16_t *bits, CellSpursEventFlagWaitMode mode) const
    { return cellSpursEventFlagWait(object_ea, bits, mode); }
    int tryWait(uint16_t *bits, CellSpursEventFlagWaitMode mode) const
    { return cellSpursEventFlagTryWait(object_ea, bits, mode); }

    int getDirection(CellSpursEventFlagDirection *direction) const
    { return cellSpursEventFlagGetDirection(object_ea, direction); }
    int getClearMode(CellSpursEventFlagClearMode *clearMode) const
    { return cellSpursEventFlagGetClearMode(object_ea, clearMode); }
    int getTasksetAddress(uint64_t *pEaTaskset) const
    { return cellSpursEventFlagGetTasksetAddress(object_ea, pEaTaskset); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif /* __cplusplus */

#else /* PPU */

/* Underlying init entry point; both Initialize wrappers below call
 * it with one of {spurs, taskset} nulled. */
extern int _cellSpursEventFlagInitialize(CellSpurs *spurs,
                                         CellSpursTaskset *taskset,
                                         CellSpursEventFlag *eventFlag,
                                         CellSpursEventFlagClearMode clearMode,
                                         CellSpursEventFlagDirection direction);

extern int cellSpursEventFlagSet(CellSpursEventFlag *ef, uint16_t bits);
extern int cellSpursEventFlagClear(CellSpursEventFlag *ef, uint16_t bits);
extern int cellSpursEventFlagWait(CellSpursEventFlag *ef, uint16_t *bits,
                                  CellSpursEventFlagWaitMode mode);
extern int cellSpursEventFlagTryWait(CellSpursEventFlag *ef, uint16_t *bits,
                                     CellSpursEventFlagWaitMode mode);

extern int cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag *ef);
extern int cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag *ef);

extern int cellSpursEventFlagGetDirection(const CellSpursEventFlag *ef,
                                          CellSpursEventFlagDirection *direction);
extern int cellSpursEventFlagGetClearMode(const CellSpursEventFlag *ef,
                                          CellSpursEventFlagClearMode *mode);
extern int cellSpursEventFlagGetTasksetAddress(const CellSpursEventFlag *ef,
                                               CellSpursTaskset **taskset);

static inline int
cellSpursEventFlagInitializeIWL(CellSpurs *spurs,
                                CellSpursEventFlag *eventFlag,
                                CellSpursEventFlagClearMode clearMode,
                                CellSpursEventFlagDirection direction)
{
    return _cellSpursEventFlagInitialize(spurs, 0, eventFlag, clearMode, direction);
}

static inline int
cellSpursEventFlagInitialize(CellSpursTaskset *taskset,
                             CellSpursEventFlag *eventFlag,
                             CellSpursEventFlagClearMode clearMode,
                             CellSpursEventFlagDirection direction)
{
    return _cellSpursEventFlagInitialize(0, taskset, eventFlag, clearMode, direction);
}

#ifdef __cplusplus
}   /* extern "C" */

namespace cell {
namespace Spurs {

class EventFlag : public CellSpursEventFlag {
public:
    static const uint32_t kAlign = CELL_SPURS_EVENT_FLAG_ALIGN;
    static const uint32_t kSize  = CELL_SPURS_EVENT_FLAG_SIZE;

    static const CellSpursEventFlagWaitMode  kOr          = CELL_SPURS_EVENT_FLAG_OR;
    static const CellSpursEventFlagWaitMode  kAnd         = CELL_SPURS_EVENT_FLAG_AND;
    static const CellSpursEventFlagClearMode kClearAuto   = CELL_SPURS_EVENT_FLAG_CLEAR_AUTO;
    static const CellSpursEventFlagClearMode kClearManual = CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL;
    static const CellSpursEventFlagDirection kSpu2Spu     = CELL_SPURS_EVENT_FLAG_SPU2SPU;
    static const CellSpursEventFlagDirection kSpu2Ppu     = CELL_SPURS_EVENT_FLAG_SPU2PPU;
    static const CellSpursEventFlagDirection kPpu2Spu     = CELL_SPURS_EVENT_FLAG_PPU2SPU;
    static const CellSpursEventFlagDirection kAny2Any     = CELL_SPURS_EVENT_FLAG_ANY2ANY;

    static int initialize(CellSpursTaskset *taskset,
                          CellSpursEventFlag *eventFlag,
                          CellSpursEventFlagClearMode clearMode,
                          CellSpursEventFlagDirection direction)
    { return cellSpursEventFlagInitialize(taskset, eventFlag, clearMode, direction); }

    static int initializeIWL(CellSpurs *spurs,
                             CellSpursEventFlag *eventFlag,
                             CellSpursEventFlagClearMode clearMode,
                             CellSpursEventFlagDirection direction)
    { return cellSpursEventFlagInitializeIWL(spurs, eventFlag, clearMode, direction); }

    int wait(uint16_t *bits, CellSpursEventFlagWaitMode mode)
    { return cellSpursEventFlagWait(this, bits, mode); }

    int tryWait(uint16_t *bits, CellSpursEventFlagWaitMode mode)
    { return cellSpursEventFlagTryWait(this, bits, mode); }

    int set(uint16_t bits)
    { return cellSpursEventFlagSet(this, bits); }

    int clear(uint16_t bits)
    { return cellSpursEventFlagClear(this, bits); }

    int attachLv2EventQueue()
    { return cellSpursEventFlagAttachLv2EventQueue(this); }

    int detachLv2EventQueue()
    { return cellSpursEventFlagDetachLv2EventQueue(this); }

    int getDirection(CellSpursEventFlagDirection *direction) const
    { return cellSpursEventFlagGetDirection(this, direction); }

    int getClearMode(CellSpursEventFlagClearMode *mode) const
    { return cellSpursEventFlagGetClearMode(this, mode); }

    int getTasksetAddress(CellSpursTaskset **taskset) const
    { return cellSpursEventFlagGetTasksetAddress(this, taskset); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_EVENT_FLAG_H__ */
