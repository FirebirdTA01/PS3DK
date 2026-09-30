/* cell/sync2/queue_types.h - the queue object and its attribute (128 bytes each).
 */
#ifndef __PS3DK_CELL_SYNC2_QUEUE_TYPES_H__
#define __PS3DK_CELL_SYNC2_QUEUE_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <cell/sync2/types.h>

#define CELL_SYNC2_QUEUE_SIZE              128
#define CELL_SYNC2_QUEUE_ALIGN             128
#define CELL_SYNC2_QUEUE_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_QUEUE_ATTRIBUTE_ALIGN   8
#define CELL_SYNC2_QUEUE_BUFFER_ALIGN      128

typedef struct CellSync2Queue {
	uint8_t skip[CELL_SYNC2_QUEUE_SIZE];
} __attribute__((aligned(CELL_SYNC2_QUEUE_ALIGN))) CellSync2Queue;

typedef struct CellSync2QueueAttribute {
	uint32_t sdkVersion;
	uint32_t threadTypes;
	size_t   elementSize;
	uint32_t depth;
	uint16_t maxPushWaiters;
	uint16_t maxPopWaiters;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_QUEUE_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) * 2 - sizeof(size_t)
	                  - sizeof(uint32_t) - sizeof(uint16_t) * 2
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_QUEUE_ATTRIBUTE_ALIGN))) CellSync2QueueAttribute;

#endif /* __PS3DK_CELL_SYNC2_QUEUE_TYPES_H__ */
