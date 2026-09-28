/* cell/spurs/trace_types.h - SPURS trace packets and trace-buffer layout.
 *
 * A SPURS trace buffer is an array of 16-byte packets.  Every packet
 * starts with an 8-byte header (tag, length in 32-bit words = 2, SPU
 * number, workload id, time = negated SPU decrementer) followed by 8
 * bytes whose meaning depends on the tag.  The PPU side fronts the
 * buffer with one CellSpursTraceInfo block.
 */
#ifndef __PS3DK_CELL_SPURS_TRACE_TYPES_H__
#define __PS3DK_CELL_SPURS_TRACE_TYPES_H__

#include <stdint.h>

typedef struct CellSpursTraceInfo {
    uint32_t spu_thread[8];         /* SPU thread id per SPU */
    uint32_t count[8];              /* packets written per SPU */
    uint32_t spu_thread_grp;
    uint32_t nspu;
    uint8_t  padding[128 - sizeof(uint32_t) * (8 + 8 + 2)];
} CellSpursTraceInfo;

typedef struct CellSpursTraceHeader {
    uint8_t  tag;
    uint8_t  length;
    uint8_t  spu;
    uint8_t  workload;
    uint32_t time;
} CellSpursTraceHeader;

/* packet tags */
#define CELL_SPURS_TRACE_TAG_KERNEL     0x20
#define CELL_SPURS_TRACE_TAG_SERVICE    0x21
#define CELL_SPURS_TRACE_TAG_TASK       0x22
#define CELL_SPURS_TRACE_TAG_JOB        0x23
#define CELL_SPURS_TRACE_TAG_OVIS       0x24
#define CELL_SPURS_TRACE_TAG_LOAD       0x2a
#define CELL_SPURS_TRACE_TAG_MAP        0x2b
#define CELL_SPURS_TRACE_TAG_START      0x2c
#define CELL_SPURS_TRACE_TAG_STOP       0x2d
#define CELL_SPURS_TRACE_TAG_USER       0x2e
#define CELL_SPURS_TRACE_TAG_GUID       0x2f
#define CELL_SPURS_TRACE_TAG_CONTROL    0xf0

typedef struct CellSpursTraceControlData {
    uint32_t incident;              /* CELL_SPURS_TRACE_CONTROL_* */
    uint32_t idSpuThread;
} CellSpursTraceControlData;

#define CELL_SPURS_TRACE_CONTROL_START  0x01
#define CELL_SPURS_TRACE_CONTROL_STOP   0x02

typedef struct CellSpursTraceServiceData {
    uint32_t incident;              /* CELL_SPURS_TRACE_SERVICE_* */
    uint32_t __reserved__;
} CellSpursTraceServiceData;

#define CELL_SPURS_TRACE_SERVICE_INIT   0x01
#define CELL_SPURS_TRACE_SERVICE_WAIT   0x02
#define CELL_SPURS_TRACE_SERVICE_EXIT   0x03

typedef struct CellSpursTraceTaskData {
    uint32_t incident;              /* CELL_SPURS_TRACE_TASK_* */
    uint32_t task;
} CellSpursTraceTaskData;

#define CELL_SPURS_TRACE_TASK_DISPATCH  0x01
#define CELL_SPURS_TRACE_TASK_YIELD     0x03
#define CELL_SPURS_TRACE_TASK_WAIT      0x04
#define CELL_SPURS_TRACE_TASK_EXIT      0x05

typedef struct CellSpursTraceJobData {
    uint8_t  __reserved__[3];
    uint8_t  binLSAhigh8;           /* bits 8..15 of the job binary's LS address */
    uint32_t jobDescriptor;
} CellSpursTraceJobData;

typedef struct CellSpursTraceLoadData {
    uint32_t ea;
    uint16_t ls;                    /* LS address >> 4 */
    uint16_t size;
} CellSpursTraceLoadData;

typedef struct CellSpursTraceMapData {
    uint32_t offset;                /* from the image's base EA */
    uint16_t ls;                    /* LS address >> 4 */
    uint16_t size;
} CellSpursTraceMapData;

typedef struct CellSpursTraceStartData {
    char     module[4];             /* four letters, e.g. "TASK" */
    uint16_t level;                 /* 1 = policy module, 2 = its work unit */
    uint16_t ls;                    /* LS address >> 2 */
} CellSpursTraceStartData;

typedef struct CellSpursTracePacket {
    CellSpursTraceHeader header;
    union {
        CellSpursTraceControlData control;
        CellSpursTraceServiceData service;
        CellSpursTraceTaskData    task;
        CellSpursTraceJobData     job;
        CellSpursTraceLoadData    load;
        CellSpursTraceMapData     map;
        CellSpursTraceStartData   start;
        uint64_t stop;
        uint64_t user;
        uint64_t guid;
        uint64_t rawData;
    } data;
} __attribute__((aligned(16))) CellSpursTracePacket;

#define CELL_SPURS_TRACE_PACKET_SIZE    16
#define CELL_SPURS_TRACE_BUFFER_ALIGN   16

/* cellSpursTraceInitialize mode flags */
#define CELL_SPURS_TRACE_MODE_FLAG_WRAP_BUFFER              0x1
#define CELL_SPURS_TRACE_MODE_FLAG_SYNCHRONOUS_START_STOP   0x2
#define CELL_SPURS_TRACE_MODE_FLAG_MASK                     0x3

#endif /* __PS3DK_CELL_SPURS_TRACE_TYPES_H__ */
