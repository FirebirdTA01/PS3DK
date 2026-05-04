/* cell/fiber/ppu_fiber_trace_types.h - scheduler trace buffer types.
 *
 * Clean-room header.  Each fiber scheduler owns a fixed-size ring
 * buffer of CellFiberPpuTracePacket entries recording dispatch /
 * resume / exit / yield / sleep events with a timebase stamp.  The
 * name-trace variant (PPU-only) extends this with 64-byte packets
 * that carry the fiber's human-readable name.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_FIBER_TRACE_TYPES_H__
#define __PS3DK_CELL_FIBER_PPU_FIBER_TRACE_TYPES_H__

#include <stdint.h>

/* ---- scheduler trace -------------------------------------------- */

typedef struct CellFiberPpuTraceInfo {
    uint32_t writeIndex;
    uint32_t numEntry;
    uint8_t  __reserved;
    uint8_t  padding[128
                     - sizeof(uint32_t) * 2
                     - sizeof(uint8_t)];
} CellFiberPpuTraceInfo;

typedef struct CellFiberPpuTraceHeader {
    uint8_t  tag;         /* CELL_FIBER_PPU_TRACE_TAG_* */
    uint8_t  length;      /* fixed at 2 */
    uint8_t  ppu;         /* PPU thread number (unused) */
    uint8_t  __reserved;  /* unused */
    uint32_t timebase;    /* lower 32 bits of TB */
} CellFiberPpuTraceHeader;

/* CellFiberPpuTracePacketHeader.tag values */
#define CELL_FIBER_PPU_TRACE_TAG_BASE      0x80

#define CELL_FIBER_PPU_TRACE_TAG_DISPATCH  (CELL_FIBER_PPU_TRACE_TAG_BASE + 0)
#define CELL_FIBER_PPU_TRACE_TAG_RESUME    (CELL_FIBER_PPU_TRACE_TAG_BASE + 1)
#define CELL_FIBER_PPU_TRACE_TAG_EXIT      (CELL_FIBER_PPU_TRACE_TAG_BASE + 2)
#define CELL_FIBER_PPU_TRACE_TAG_YIELD     (CELL_FIBER_PPU_TRACE_TAG_BASE + 3)
#define CELL_FIBER_PPU_TRACE_TAG_SLEEP     (CELL_FIBER_PPU_TRACE_TAG_BASE + 4)
#define CELL_FIBER_PPU_TRACE_TAG_NAME      (CELL_FIBER_PPU_TRACE_TAG_BASE + 5)

typedef struct CellFiberPpuTraceSchedulerPayload {
    uint32_t idFiber;
    uint32_t idPpuThread;
} CellFiberPpuTraceSchedulerPayload;

/* Portable alignment wrapper — GCC vs MSVC. */
#ifdef __GNUC__
#define CELL_FIBER_ALIGN_PREFIX(x)
#define CELL_FIBER_ALIGN_POSTFIX(x)  __attribute__((aligned(x)))
#else
#define CELL_FIBER_ALIGN_PREFIX(x)   __declspec(align(x))
#define CELL_FIBER_ALIGN_POSTFIX(x)
#endif

typedef CELL_FIBER_ALIGN_PREFIX(16) struct CellFiberPpuTracePacket {
    CellFiberPpuTraceHeader           header;
    CellFiberPpuTraceSchedulerPayload data;
} CELL_FIBER_ALIGN_POSTFIX(16) CellFiberPpuTracePacket;

#define CELL_FIBER_PPU_TRACE_PACKET_SIZE   16
#define CELL_FIBER_PPU_TRACE_BUFFER_ALIGN  16

#define CELL_FIBER_PPU_TRACE_MODE_FLAG_WRAP_BUFFER           0x1
#define CELL_FIBER_PPU_TRACE_MODE_FLAG_SYNCHRONOUS_START_STOP 0x2
#define CELL_FIBER_PPU_TRACE_MODE_FLAG_MASK                  0x3

/* ---- name trace (PPU-only) -------------------------------------- */

#ifdef __PPU__

typedef CELL_FIBER_ALIGN_PREFIX(16) struct CellFiberPpuNameTraceInfo {
    uint32_t write;
    uint32_t read;
    uint32_t last;
    uint32_t mode;           /* 0 = stop, 1 = overflow */
    uint32_t is_started;
    uint8_t  padding[128 - sizeof(uint32_t) * 5];
} CellFiberPpuNameTraceInfo CELL_FIBER_ALIGN_POSTFIX(16);

/* Need CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH from context types. */
#include <cell/fiber/ppu_context_types.h>

typedef CELL_FIBER_ALIGN_PREFIX(16) struct CellFiberPpuNameTracePacket {
    CellFiberPpuTraceHeader header;               /* 8 bytes */
    uint32_t                idFiber;
    char                    name[CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH + 1]; /* 32 bytes */
    char                    __pad__[64
                                    - sizeof(CellFiberPpuTraceHeader)
                                    - sizeof(uint32_t)
                                    - (CELL_FIBER_PPU_CONTEXT_NAME_MAX_LENGTH + 1)];
} CellFiberPpuNameTracePacket CELL_FIBER_ALIGN_POSTFIX(16);

#endif /* __PPU__ */

#endif /* __PS3DK_CELL_FIBER_PPU_FIBER_TRACE_TYPES_H__ */
