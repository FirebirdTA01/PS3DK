#ifndef __PS3DK_CELL_SWCACHE_PATCH_OBJECT_H__
#define __PS3DK_CELL_SWCACHE_PATCH_OBJECT_H__

#include <cell/swcache/default_cache.h>

#ifndef __CELL_SWCACHE_ASSERT
#define __CELL_SWCACHE_ASSERT(cond, ...) ((void)sizeof(cond))
#endif

#define CELL_SWCACHE_PATCH_OBJECT(type, ptr) \
    do { (void)sizeof(type); (void)(ptr); } while (0)

#define CELL_SWCACHE_PATCH_CONST_OBJECT(type, ptr) \
    do { (void)sizeof(type); (void)(ptr); } while (0)

#define CELL_SWCACHE_PATCH_OBJECT_AND_BLOCK(type, ptr) \
    do { (void)sizeof(type); (void)(ptr); } while (0)

#define CELL_SWCACHE_PATCH_CONST_OBJECT_AND_BLOCK(type, ptr) \
    do { (void)sizeof(type); (void)(ptr); } while (0)

#define CELL_SWCACHE_PATCH_ARRAY_AND_BLOCK(type, ptr, count) \
    do { (void)sizeof(type); (void)(ptr); (void)(count); } while (0)

#define CELL_SWCACHE_PATCH_VCLASS(type, ptr) \
    do { (void)sizeof(type); (void)(ptr); } while (0)

#define CELL_SWCACHE_REPATCH(ptr) \
    do { (void)(ptr); } while (0)

#define CELL_SWCACHE_REPATCH_AND_BLOCK(ptr) \
    do { (void)(ptr); } while (0)

#define CELL_SWCACHE_WAIT do { } while (0)

#endif /* __PS3DK_CELL_SWCACHE_PATCH_OBJECT_H__ */
