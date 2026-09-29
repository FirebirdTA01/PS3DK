/* cell/sync2/cond_types.h - the cond object and its attribute (128 bytes each).
 */
#ifndef __PS3DK_CELL_SYNC2_COND_TYPES_H__
#define __PS3DK_CELL_SYNC2_COND_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <cell/sync2/types.h>

#define CELL_SYNC2_COND_SIZE              128
#define CELL_SYNC2_COND_ALIGN             128
#define CELL_SYNC2_COND_ATTRIBUTE_SIZE    128
#define CELL_SYNC2_COND_ATTRIBUTE_ALIGN   8

typedef struct CellSync2Cond {
	uint8_t skip[CELL_SYNC2_COND_SIZE];
} __attribute__((aligned(CELL_SYNC2_COND_ALIGN))) CellSync2Cond;

typedef struct CellSync2CondAttribute {
	uint32_t sdkVersion;
	uint16_t maxWaiters;
	char     name[CELL_SYNC2_NAME_MAX_LENGTH + 1];
	uint8_t  reserved[CELL_SYNC2_COND_ATTRIBUTE_SIZE
	                  - sizeof(uint32_t) - sizeof(uint16_t)
	                  - (CELL_SYNC2_NAME_MAX_LENGTH + 1)];
} __attribute__((aligned(CELL_SYNC2_COND_ATTRIBUTE_ALIGN))) CellSync2CondAttribute;

#endif /* __PS3DK_CELL_SYNC2_COND_TYPES_H__ */
