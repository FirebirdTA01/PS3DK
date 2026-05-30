#ifndef __PS3DK_SCE_HL_SPURS_SPURS_H__
#define __PS3DK_SCE_HL_SPURS_SPURS_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/spu_thread_group.h>
#include <sys/types.h>
#include <cell/spurs.h>
#include <cell/spurs/exception_types.h>
#include <sce/hl/spurs/define.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int cellSpursEnableExceptionEventHandler(CellSpurs *spurs, bool enable);
extern int cellSpursSetExceptionEventHandler(CellSpurs *spurs,
                                             CellSpursWorkloadId wid,
                                             CellSpursExceptionEventHandler handler,
                                             void *arg);
extern int cellSpursUnsetExceptionEventHandler(CellSpurs *spurs,
                                               CellSpursWorkloadId wid);
extern int cellSpursSetGlobalExceptionEventHandler(
    CellSpurs *spurs,
    CellSpursGlobalExceptionEventHandler handler,
    void *arg);
extern int cellSpursUnsetGlobalExceptionEventHandler(CellSpurs *spurs);
extern int cellSpursSetPreemptionVictimHints(CellSpurs *spurs,
                                             const bool isPreemptible[8]);

#ifdef __cplusplus
}

__SCE_HL_SPURS_BEGIN

class Spurs : public cell::Spurs::Spurs {
public:
    enum Type {
        TYPE_EXCLUSIVE_NON_CONTEXT = 0,
        TYPE_SHARED_WITH_ANY = 1,
        TYPE_SHARED_WITH_HIGHER = 2
    };

    static const uint32_t DEFAULT_NUM_SPUS = 5;
    static const int DEFAULT_PPU_THREAD_PRIORITY = 1000;
    static const int DEFAULT_SPU_THREAD_GROUP_PRIORITY = 250;
    static const uint32_t ALIGN = CELL_SPURS_ALIGN;

    Spurs() {}

    static int initialize(Spurs *spurs,
                          const char *prefix,
                          uint32_t nSpus = DEFAULT_NUM_SPUS,
                          int spuPriority = DEFAULT_SPU_THREAD_GROUP_PRIORITY,
                          int ppuPriority = DEFAULT_PPU_THREAD_PRIORITY,
                          Type type = TYPE_EXCLUSIVE_NON_CONTEXT)
    {
        cell::Spurs::SpursAttribute attr;
        __SCE_SPURS_UTIL_RETURN_IF(cell::Spurs::SpursAttribute::initialize(
            &attr, nSpus, spuPriority, ppuPriority, false));
        __SCE_SPURS_UTIL_RETURN_IF(attr.enableSpuPrintfIfAvailable());
        if (prefix) {
            __SCE_SPURS_UTIL_RETURN_IF(attr.setNamePrefix(prefix, __builtin_strlen(prefix)));
        }
        __SCE_SPURS_UTIL_RETURN_IF(attr.setSpuThreadGroupType(groupType(type)));
        return cell::Spurs::Spurs::initializeWithAttribute(spurs, &attr);
    }

    static int initialize(Spurs *spurs,
                          const char *prefix,
                          uint32_t nSpus,
                          int spuPriority,
                          int ppuPriority,
                          bool exitIfNoWork,
                          sys_memory_container_t container)
    {
        cell::Spurs::SpursAttribute attr;
        __SCE_SPURS_UTIL_RETURN_IF(cell::Spurs::SpursAttribute::initialize(
            &attr, nSpus, spuPriority, ppuPriority, exitIfNoWork));
        __SCE_SPURS_UTIL_RETURN_IF(attr.enableSpuPrintfIfAvailable());
        if (prefix) {
            __SCE_SPURS_UTIL_RETURN_IF(attr.setNamePrefix(prefix, __builtin_strlen(prefix)));
        }
        __SCE_SPURS_UTIL_RETURN_IF(attr.setMemoryContainerForSpuThread(container));
        return cell::Spurs::Spurs::initializeWithAttribute(spurs, &attr);
    }

    int enableExceptionEventHandler()
    { return cellSpursEnableExceptionEventHandler(this, true); }

    int disableExceptionEventHandler()
    { return cellSpursEnableExceptionEventHandler(this, false); }

    int setExceptionEventHandler(CellSpursWorkloadId wid,
                                 CellSpursExceptionEventHandler handler,
                                 void *arg)
    { return cellSpursSetExceptionEventHandler(this, wid, handler, arg); }

    int unsetExceptionEventHandler(CellSpursWorkloadId wid)
    { return cellSpursUnsetExceptionEventHandler(this, wid); }

    int setGlobalExceptionEventHandler(CellSpursGlobalExceptionEventHandler handler,
                                       void *arg)
    { return cellSpursSetGlobalExceptionEventHandler(this, handler, arg); }

    int unsetGlobalExceptionEventHandler()
    { return cellSpursUnsetGlobalExceptionEventHandler(this); }

    int setPreemptionVictimHints(const bool isPreemptible[8])
    { return cellSpursSetPreemptionVictimHints(this, isPreemptible); }

private:
    static int groupType(Type type)
    {
        switch (type) {
        case TYPE_SHARED_WITH_ANY:
            return SYS_SPU_THREAD_GROUP_TYPE_NORMAL;
        case TYPE_SHARED_WITH_HIGHER:
            return SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM;
        case TYPE_EXCLUSIVE_NON_CONTEXT:
        default:
            return SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT;
        }
    }
} __attribute__((aligned(CELL_SPURS_ALIGN)));

__SCE_HL_SPURS_END

#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_SPURS_H__ */
