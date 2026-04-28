/* cell/trace/trace_types.h - SPURS / SPU trace-packet header.
 *
 * 8-byte packet preamble that fronts every trace record the SPU-side
 * SPURS runtime emits.  Consumed by trace-collection tooling and by
 * the SPURS job-queue trace-dump path.
 */
#ifndef __PS3DK_CELL_TRACE_TRACE_TYPES_H__
#define __PS3DK_CELL_TRACE_TRACE_TYPES_H__

#include <stdint.h>

typedef struct __attribute__((aligned(8))) CellTraceHeader {
    uint8_t  tag;     /* packet tag */
    uint8_t  length;  /* payload length in 32-bit words */
    uint8_t  cpu;     /* originating processor id */
    uint8_t  thread;  /* originating thread id */
    uint32_t time;    /* low 32 bits of the timebase */
} CellTraceHeader;

#endif /* __PS3DK_CELL_TRACE_TRACE_TYPES_H__ */
