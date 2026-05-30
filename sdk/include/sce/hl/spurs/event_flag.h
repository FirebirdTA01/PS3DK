#ifndef __PS3DK_SCE_HL_SPURS_EVENT_FLAG_H__
#define __PS3DK_SCE_HL_SPURS_EVENT_FLAG_H__

#include <stdint.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>
#include <sce/hl/spurs/spurs.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

class EventFlag : public CellSpursEventFlag {
public:
    static const uint32_t ALIGN = CELL_SPURS_EVENT_FLAG_ALIGN;

    static int initialize(EventFlag *eventFlag, const Spurs *spurs)
    {
        return cellSpursEventFlagInitializeIWL(
            const_cast<Spurs *>(spurs),
            eventFlag,
            CELL_SPURS_EVENT_FLAG_CLEAR_AUTO,
            CELL_SPURS_EVENT_FLAG_ANY2ANY);
    }

    int wait(uint16_t *mask,
             CellSpursEventFlagWaitMode mode = CELL_SPURS_EVENT_FLAG_OR)
    { return cellSpursEventFlagWait(this, mask, mode); }

    int tryWait(uint16_t *mask,
                CellSpursEventFlagWaitMode mode = CELL_SPURS_EVENT_FLAG_OR)
    { return cellSpursEventFlagTryWait(this, mask, mode); }

    int attachLv2EventQueue()
    { return cellSpursEventFlagAttachLv2EventQueue(this); }

    int detachLv2EventQueue()
    { return cellSpursEventFlagDetachLv2EventQueue(this); }

    int set(uint16_t bits) { return cellSpursEventFlagSet(this, bits); }
    int clear(uint16_t bits) { return cellSpursEventFlagClear(this, bits); }

    int getDirection(CellSpursEventFlagDirection *direction) const
    { return cellSpursEventFlagGetDirection(this, direction); }

    int getClearMode(CellSpursEventFlagClearMode *mode) const
    { return cellSpursEventFlagGetClearMode(this, mode); }

    int getTasksetAddress(CellSpursTaskset **taskset) const
    { return cellSpursEventFlagGetTasksetAddress(this, taskset); }
} __attribute__((aligned(CELL_SPURS_EVENT_FLAG_ALIGN)));

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_EVENT_FLAG_H__ */
