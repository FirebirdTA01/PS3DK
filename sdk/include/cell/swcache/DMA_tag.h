#ifndef __PS3DK_CELL_SWCACHE_DMA_TAG_H__
#define __PS3DK_CELL_SWCACHE_DMA_TAG_H__

#include <cell/swcache/define.h>

#define CELL_SWCACHE_DMA_TAG_NONE 0u

#ifndef CELL_SWCACHE_DMA_WAIT_TAG_STATUS_ALL
#define CELL_SWCACHE_DMA_WAIT_TAG_STATUS_ALL(mask) ((void)(mask), 0u)
#endif

#endif /* __PS3DK_CELL_SWCACHE_DMA_TAG_H__ */
