#ifndef __PS3DK_CELL_SWCACHE_DEFINE_H__
#define __PS3DK_CELL_SWCACHE_DEFINE_H__

#include <stdint.h>

#ifndef CBE_CACHE_LINE
#define CBE_CACHE_LINE 128
#endif

#ifndef __ALIGNED_CBE_CACHE_LINE__
#define __ALIGNED_CBE_CACHE_LINE__ __attribute__((aligned(CBE_CACHE_LINE)))
#endif

#define CELL_SWCACHE_ERROR_NULL_POINTER       (-1)
#define CELL_SWCACHE_ERROR_NON_CACHED_POINTER (-2)

#ifdef __cplusplus
namespace cell {
namespace swcache {

enum CacheStatus {
    CACHED = 0,
    IS_WRITABLE = 1
};

typedef uint32_t DMA_TAG_T;

} /* namespace swcache */
} /* namespace cell */
#endif

#endif /* __PS3DK_CELL_SWCACHE_DEFINE_H__ */
