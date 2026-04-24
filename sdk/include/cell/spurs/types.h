/* cell/spurs/types.h - SPURS opaque types + C++ class wrappers.
 *
 * Clean-room surface matching the reference SDK's cell/spurs/types.h
 * ABI for the C-level CellSpursAttribute / CellSpurs / CellSpurs2
 * byte-array containers, plus the `cell::Spurs::*` C++ class
 * wrappers that reference-SDK samples use.  The C++ classes inherit
 * the byte-array types and add inline methods that forward to the C
 * entry points exported by libspurs_stub.a.  Zero additional data
 * members - the byte-array size is authoritative.
 */
#ifndef __PS3DK_CELL_SPURS_TYPES_H__
#define __PS3DK_CELL_SPURS_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <ppu-types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SPURS_MAX_SPU                 8
#define CELL_SPURS_NAME_MAX_LENGTH         15

#define CELL_SPURS_MAX_WORKLOAD            16
#define CELL_SPURS_MAX_WORKLOAD1           16
#define CELL_SPURS_MAX_WORKLOAD2           32
#define CELL_SPURS_MAX_PRIORITY            16

#define CELL_SPURS_ATTRIBUTE_ALIGN         8
#define CELL_SPURS_ATTRIBUTE_SIZE          512
#define CELL_SPURS_ALIGN                   128
#define CELL_SPURS_SIZE                    4096
#define CELL_SPURS_SIZE2                   8192

typedef unsigned CellSpursWorkloadId;

typedef struct CellSpursAttribute {
    unsigned char skip[CELL_SPURS_ATTRIBUTE_SIZE];
} __attribute__((aligned(CELL_SPURS_ATTRIBUTE_ALIGN))) CellSpursAttribute;

typedef struct CellSpurs {
    unsigned char skip[CELL_SPURS_SIZE];
} __attribute__((aligned(CELL_SPURS_ALIGN))) CellSpurs;

/* CellSpurs2 has the same alignment as CellSpurs and is twice the
 * byte-array size.  In C++ it's derived from cell::Spurs::Spurs to
 * allow polymorphic passing to functions that take a CellSpurs* - the
 * extra bytes lie immediately after the inherited 4096-byte region.
 * The C-only branch is a flat 8192-byte container. */
#ifndef __cplusplus
typedef struct CellSpurs2 {
    unsigned char skip[CELL_SPURS_SIZE2];
} __attribute__((aligned(CELL_SPURS_ALIGN))) CellSpurs2;
#endif

/* C entry points consumed by the C++ wrappers + direct-C callers.
 * All resolve to NIDs in libspurs_stub.a. */

#define _CELL_SPURS_ATTRIBUTE_REVISION     0x02
#define _CELL_SPURS_INTERNAL_VERSION       0x330000

extern int _cellSpursAttributeInitialize(CellSpursAttribute *attr,
                                         unsigned int revision,
                                         unsigned int sdkVersion,
                                         unsigned int nSpus,
                                         int spuPriority,
                                         int ppuPriority,
                                         bool exitIfNoWork);
extern int cellSpursAttributeSetNamePrefix(CellSpursAttribute *attr,
                                           const char *name,
                                           size_t size);
extern int cellSpursAttributeSetMemoryContainerForSpuThread(
                                           CellSpursAttribute *attr,
                                           sys_memory_container_t container);
extern int cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute *attr,
                                                   int type);
extern int cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute *attr);
extern int cellSpursAttributeEnableSystemWorkload(CellSpursAttribute *attr,
                                                  const uint8_t priority[8],
                                                  unsigned int maxSpu,
                                                  const bool isPreemptible[8]);

extern int cellSpursInitialize(CellSpurs *spurs,
                               unsigned int nSpus,
                               int spuPriority,
                               int ppuPriority,
                               bool exitIfNoWork);
extern int cellSpursInitializeWithAttribute(CellSpurs *spurs,
                                            const CellSpursAttribute *attr);
extern int cellSpursInitializeWithAttribute2(CellSpurs *spurs,
                                             const CellSpursAttribute *attr);
extern int cellSpursFinalize(CellSpurs *spurs);
extern int cellSpursWakeUp(CellSpurs *spurs);
extern int cellSpursGetNumSpuThread(CellSpurs *spurs, unsigned int *nThreads);
extern int cellSpursGetSpuThreadId(CellSpurs *spurs,
                                   sys_spu_thread_t *threads,
                                   unsigned int *nThreads);
extern int cellSpursGetSpuThreadGroupId(CellSpurs *spurs,
                                        sys_spu_group_t *groupId);

static inline int cellSpursAttributeInitialize(CellSpursAttribute *attr,
                                               unsigned int nSpus,
                                               int spuPriority,
                                               int ppuPriority,
                                               bool exitIfNoWork)
{
    return _cellSpursAttributeInitialize(attr,
                                         _CELL_SPURS_ATTRIBUTE_REVISION,
                                         _CELL_SPURS_INTERNAL_VERSION,
                                         nSpus, spuPriority, ppuPriority,
                                         exitIfNoWork);
}

#ifdef __cplusplus
}   /* extern "C" */

/* C++ class wrappers - inline forwarders over the C entry points.  No
 * extra data members; these are pure methods living on top of the
 * inherited byte-array container. */

namespace cell {
namespace Spurs {

class SpursAttribute : public CellSpursAttribute {
public:
    static int initialize(SpursAttribute *attribute,
                          unsigned int nSpus,
                          int spuPriority,
                          int ppuPriority,
                          bool exitIfNoWork)
    {
        return cellSpursAttributeInitialize(attribute, nSpus,
                                            spuPriority, ppuPriority,
                                            exitIfNoWork);
    }

    int setNamePrefix(const char *name, size_t size)
    { return cellSpursAttributeSetNamePrefix(this, name, size); }

    int setMemoryContainerForSpuThread(sys_memory_container_t container)
    { return cellSpursAttributeSetMemoryContainerForSpuThread(this, container); }

    int setSpuThreadGroupType(int type)
    { return cellSpursAttributeSetSpuThreadGroupType(this, type); }

    int enableSpuPrintfIfAvailable()
    { return cellSpursAttributeEnableSpuPrintfIfAvailable(this); }

    int enableSystemWorkload(const uint8_t priority[8],
                             unsigned int maxSpu,
                             const bool isPreemptible[8])
    { return cellSpursAttributeEnableSystemWorkload(this, priority, maxSpu, isPreemptible); }
};

class Spurs : public CellSpurs {
public:
    static const uint32_t kMaxWorkload = CELL_SPURS_MAX_WORKLOAD;
    static const uint32_t kMaxPriority = CELL_SPURS_MAX_PRIORITY;
    static const uint32_t kAlign       = CELL_SPURS_ALIGN;
    static const uint32_t kSize        = CELL_SPURS_SIZE;

    static int initialize(Spurs *spurs,
                          unsigned int nSpus,
                          int spuPriority,
                          int ppuPriority,
                          bool exitIfNoWork)
    { return cellSpursInitialize(spurs, nSpus, spuPriority, ppuPriority, exitIfNoWork); }

    static int initialize(Spurs *spurs, const CellSpursAttribute *attr)
    { return cellSpursInitializeWithAttribute(spurs, attr); }

    static int initializeWithAttribute(Spurs *spurs, const CellSpursAttribute *attr)
    { return cellSpursInitializeWithAttribute(spurs, attr); }

    int finalize()
    { return cellSpursFinalize(this); }

    int wakeUp()
    { return cellSpursWakeUp(this); }

    int getNumSpuThread(unsigned int *nThreads)
    { return cellSpursGetNumSpuThread(this, nThreads); }

    int getSpuThreadId(sys_spu_thread_t *threads, unsigned int *nThreads)
    { return cellSpursGetSpuThreadId(this, threads, nThreads); }

    int getSpuThreadGroupId(sys_spu_group_t *groupId)
    { return cellSpursGetSpuThreadGroupId(this, groupId); }
};

}   /* namespace Spurs */
}   /* namespace cell */

/* CellSpurs2 lives outside namespace cell::Spurs in the reference
 * SDK; it's a plain struct that extends cell::Spurs::Spurs with extra
 * bytes so cellSpursInitializeWithAttribute2 can fill a larger region
 * while the base CellSpurs view still works. */
struct CellSpurs2 : public cell::Spurs::Spurs {
    unsigned char skip[CELL_SPURS_SIZE2 - CELL_SPURS_SIZE];
} __attribute__((aligned(CELL_SPURS_ALIGN)));

namespace cell {
namespace Spurs {

class Spurs2 : public ::CellSpurs2 {
public:
    static const uint32_t kMaxWorkload = CELL_SPURS_MAX_WORKLOAD2;
    static const uint32_t kSize        = CELL_SPURS_SIZE2;

    static int initialize(Spurs2 *spurs2, const CellSpursAttribute *attr)
    { return cellSpursInitializeWithAttribute2(spurs2, attr); }

    static int initializeWithAttribute2(Spurs2 *spurs2, const CellSpursAttribute *attr)
    { return cellSpursInitializeWithAttribute2(spurs2, attr); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif /* __cplusplus */

#endif /* __PS3DK_CELL_SPURS_TYPES_H__ */
