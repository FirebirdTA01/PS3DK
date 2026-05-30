#ifndef __PS3DK_CELL_SWCACHE_DEFAULT_CACHE_H__
#define __PS3DK_CELL_SWCACHE_DEFAULT_CACHE_H__

#include <stddef.h>
#include <stdint.h>

#include <cell/swcache/define.h>

#ifdef __cplusplus
namespace cell {
namespace swcache {

class DefaultHeap {
public:
    static void initialize(void *heap, size_t size)
    {
        (void)heap;
        (void)size;
    }

    static void finalize(void)
    {
    }
};

template <class Heap>
class DefaultCache {
public:
    enum {
        SIZE_OF_DIR = 256
    };

    class MemBlockHeader {
    public:
        static MemBlockHeader *getHeader(void *ptr)
        {
            return reinterpret_cast<MemBlockHeader *>(ptr);
        }

        void releaseObject(uint32_t tag)
        {
            (void)tag;
        }
    };

    static void initialize(void *heap, size_t size)
    {
        Heap::initialize(heap, size);
    }

    static void finalize(uint32_t tag = 0)
    {
        (void)tag;
        Heap::finalize();
    }

    static int query(void *ptr, size_t size, uint32_t &tag, uint32_t &ea)
    {
        (void)size;
        tag = 0;
        ea = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
        return ptr ? CELL_SWCACHE_ERROR_NON_CACHED_POINTER
                   : CELL_SWCACHE_ERROR_NULL_POINTER;
    }

    static int grabWithNoAlloc(void *ls, uint32_t ea, size_t size,
                               uint32_t flags, uint32_t tag,
                               uint32_t wait)
    {
        (void)ea;
        (void)size;
        (void)flags;
        (void)tag;
        (void)wait;
        return ls ? CACHED : CELL_SWCACHE_ERROR_NULL_POINTER;
    }

    static void waitForGrab(void *ptr)
    {
        (void)ptr;
    }

    static void flushAll(uint32_t tag = 0)
    {
        (void)tag;
    }

    static void refreshAll(uint32_t tag = 0)
    {
        (void)tag;
    }
};

template <class Cache>
class CacheResource {
public:
    static void initialize(void *heap, size_t size)
    {
        Cache::initialize(heap, size);
    }

    static void finalize(uint32_t tag = 0)
    {
        Cache::finalize(tag);
    }
};

} /* namespace swcache */
} /* namespace cell */
#endif

#endif /* __PS3DK_CELL_SWCACHE_DEFAULT_CACHE_H__ */
