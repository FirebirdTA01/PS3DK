#ifndef __PS3DK_CELL_SWCACHE_TYPES_H__
#define __PS3DK_CELL_SWCACHE_TYPES_H__

#include <stddef.h>
#include <stdint.h>

#include <cell/swcache/define.h>

#ifdef __cplusplus
namespace cell {
namespace swcache {

typedef void Vtab;

struct Vtab2id {
    uintptr_t vtab;
    uint16_t id;
    uint16_t reserved;
};

class ClassIdTabHeader {
public:
    typedef Vtab *(*BuildVtableFunc)(uint16_t id, Vtab *vtab);

    static void setFpBuildVtable(BuildVtableFunc func)
    {
        buildVtableFunc() = func;
    }

    static void setClassIdTabOnSPU(const void *table)
    {
        classIdTab() = table;
    }

    static Vtab *buildVtable(uint16_t id, Vtab *vtab, size_t objectSize, ...)
    {
        (void)id;
        (void)objectSize;
        return vtab;
    }

private:
    static BuildVtableFunc &buildVtableFunc()
    {
        static BuildVtableFunc func = 0;
        return func;
    }

    static const void *&classIdTab()
    {
        static const void *table = 0;
        return table;
    }
};

} /* namespace swcache */
} /* namespace cell */
#endif

#endif /* __PS3DK_CELL_SWCACHE_TYPES_H__ */
