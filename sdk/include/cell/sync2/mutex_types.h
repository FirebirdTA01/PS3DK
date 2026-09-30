/* cell/sync2/mutex_types.h - the mutex object and its attribute (128 bytes each).
 */
#ifndef __PS3DK_CELL_SYNC2_MUTEX_TYPES_H__
#define __PS3DK_CELL_SYNC2_MUTEX_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <cell/sync2/types.h>

#define CELL_SYNC2_MUTEX_SIZE              128
#define CELL_SYNC2_MUTEX_ALIGN             128
#define CELL_SYNC2_MUTEX_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_MUTEX_ATTRIBUTE_ALIGN   8

typedef struct CellSync2Mutex {
	uint8_t skip[CELL_SYNC2_MUTEX_SIZE];
} __attribute__((aligned(CELL_SYNC2_MUTEX_ALIGN))) CellSync2Mutex;

typedef struct CellSync2MutexAttribute {
	uint32_t sdkVersion;
	uint16_t threadTypes;
	uint16_t maxWaiters;
	bool     recursive;
	uint8_t  padding;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_MUTEX_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) - sizeof(uint16_t) * 2
	                  - sizeof(bool) - sizeof(uint8_t)
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_MUTEX_ATTRIBUTE_ALIGN))) CellSync2MutexAttribute;

#endif /* __PS3DK_CELL_SYNC2_MUTEX_TYPES_H__ */
