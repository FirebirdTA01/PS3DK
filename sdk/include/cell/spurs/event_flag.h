/* cell/spurs/event_flag.h - SPURS event-flag sync primitive.
 *
 * Clean-room header.  CellSpursEventFlag is a 128-byte opaque block
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
#endif

#endif /* __PS3DK_CELL_SPURS_EVENT_FLAG_H__ */
