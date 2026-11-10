/*
 * PS3 Custom Toolchain — sdk/include/cell/gcm/gcm_segment.h
 *
 * Label-fenced GCM command segments: small command buffers outside the
 * main ring that the ring runs with a CALL and that end in a RETURN.
 * Rules, limits and measured behaviour: docs/gcm-command-segments.md.
 *
 *   CellGcmSegment seg[2];
 *   cellGcmSegmentInit(&seg[0], buf0, 64 * 1024, CELL_GCM_LOCATION_MAIN, LABEL_A);
 *   ...
 *   CellGcmSegment *s = &seg[frame & 1];
 *   if (cellGcmSegmentWaitRetired(s, 2000, 500, NULL) != CELL_OK) fail();
 *   CellGcmContextData *c = cellGcmSegmentBegin(s);   any cellGcm or rsx emitter
 *   cellGcmSetClearColor(c, 0xff2040c0);              writes into the segment
 *   cellGcmSetClearSurface(c, CELL_GCM_CLEAR_R | ...);
 *   if (cellGcmSegmentEnd(s) != CELL_OK) fail();       overflow is refused here
 *   cellGcmSegmentCall(ring, s);                       CALL + retirement label
 *
 * The segment context is an ordinary CellGcmContextData set up with
 * cellGcmSetupContextData over the segment buffer; its callback never
 * wraps or flushes.  When an emitter needs more room than is left, the
 * callback records the words that did not fit and refuses, so that
 * command is not written (an emitter that writes in several reservations
 * may have written its earlier parts), and cellGcmSegmentEnd then returns
 * CELL_GCM_SEGMENT_ERROR_OVERFLOW.  An overflowed segment can never be
 * called.  The last word of the buffer is kept for the RETURN, so a
 * segment that ends without overflow always has room for it.
 *
 * Retirement: cellGcmSegmentCall writes, in the ring after the CALL, a
 * back-end label (cellGcmSetWriteBackEndLabel) with the segment's next
 * serial.  The back end writes it after the work before it, including the
 * segment's, has completed, so a segment is reused only once its label has
 * reached that serial.  Serial comparison is modulo 2^32.
 *
 * Header-only declarations; the code is in libgcm_cmd.a (gcm_segment.c).
 */

#ifndef PS3TC_CELL_GCM_SEGMENT_H
#define PS3TC_CELL_GCM_SEGMENT_H

#include <stdint.h>
#include <cell/error.h>
#include <rsx/gcm_sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The context type the functions take and return.  It is the same
 * struct in both languages; C++ code gets the cell spelling. */
#if defined(__cplusplus) && defined(__CELL_GCM_H__)
typedef CellGcmContextData CellGcmSegmentContext;
#else
typedef gcmContextData CellGcmSegmentContext;
#endif

/* SDK-specific status codes (CELL_OK on success). */
#define CELL_GCM_SEGMENT_ERROR_INVALID_VALUE  ((int32_t)0x802b0f01)
#define CELL_GCM_SEGMENT_ERROR_BAD_ADDRESS    ((int32_t)0x802b0f02)
#define CELL_GCM_SEGMENT_ERROR_BUSY           ((int32_t)0x802b0f03)
#define CELL_GCM_SEGMENT_ERROR_STATE          ((int32_t)0x802b0f04)
#define CELL_GCM_SEGMENT_ERROR_OVERFLOW       ((int32_t)0x802b0f05)
#define CELL_GCM_SEGMENT_ERROR_TIMEOUT        ((int32_t)0x802b0f06)

/* Limits.  The CALL word carries a 4-byte-aligned offset below 512 MiB
 * (mask 0x1ffffffc).  The helper asks for 128-byte alignment so that a
 * segment is also a whole number of SPU DMA lines. */
#define CELL_GCM_SEGMENT_ALIGN        128u
#define CELL_GCM_SEGMENT_MIN_SIZE     128u
#define CELL_GCM_SEGMENT_OFFSET_LIMIT 0x20000000u

enum {
    CELL_GCM_SEGMENT_STATE_IDLE      = 0, /* retired or never used        */
    CELL_GCM_SEGMENT_STATE_OPEN      = 1, /* between Begin and End        */
    CELL_GCM_SEGMENT_STATE_CLOSED    = 2, /* ended, not yet called        */
    CELL_GCM_SEGMENT_STATE_SUBMITTED = 3  /* called, label not yet seen   */
};

typedef struct CellGcmSegment {
    gcmContextData context;     /* first member: the overflow callback casts */
    uint32_t *buffer;           /* segment storage (main or RSX local)       */
    uint32_t sizeBytes;         /* storage size, multiple of 128             */
    uint32_t offset;            /* RSX offset the CALL targets               */
    uint8_t  location;          /* CELL_GCM_LOCATION_MAIN / _LOCAL           */
    uint8_t  labelIndex;        /* retirement label                          */
    uint8_t  state;             /* CELL_GCM_SEGMENT_STATE_*                  */
    uint8_t  reserved;
    uint32_t serial;            /* serial of the last call (label target)    */
    uint32_t usedBytes;         /* bytes written by the last End, RETURN incl */
    uint32_t overflowWords;     /* words refused since Begin, 0 = none       */
    uint32_t calls;             /* calls since Init                          */
} CellGcmSegment;

/* Prepare a segment over `buffer`, which must already be RSX-visible:
 * main memory mapped with cellGcmMapMainMemory (or inside the cellGcmInit
 * IO region), or RSX local memory.  Sets the label to 0 (retired). */
int32_t cellGcmSegmentInit(CellGcmSegment *segment, void *buffer,
                           uint32_t sizeBytes, uint8_t location,
                           uint8_t labelIndex);

/* 1 when the segment may be rebuilt (idle or its label reached its
 * serial), else 0. */
int32_t cellGcmSegmentIsRetired(CellGcmSegment *segment);

/* Wait at most maxPolls polls, sleeping pollMicroseconds between them,
 * for the segment to retire.  CELL_OK, or CELL_GCM_SEGMENT_ERROR_TIMEOUT
 * with the segment unchanged.  *pollsUsed (optional) gets the polls that
 * found it busy. */
int32_t cellGcmSegmentWaitRetired(CellGcmSegment *segment, uint32_t maxPolls,
                                  uint32_t pollMicroseconds,
                                  uint32_t *pollsUsed);

/* Open the segment for writing.  NULL when it is still in flight or
 * already open. */
CellGcmSegmentContext *cellGcmSegmentBegin(CellGcmSegment *segment);

/* Append the RETURN and close the segment.  CELL_GCM_SEGMENT_ERROR_OVERFLOW
 * if any emitter was refused since Begin (the segment is closed but can
 * not be called; Begin it again). */
int32_t cellGcmSegmentEnd(CellGcmSegment *segment);

/* A segment written by someone else (an SPU job, a DMA): BeginExternal
 * claims a retired segment and returns its buffer (NULL when in flight);
 * once the writer is done and its data has reached memory, EndExternal
 * closes it.  `usedBytes` is what was written, a multiple of 4 within the
 * buffer whose last word is the RETURN; anything else is refused with
 * CELL_GCM_SEGMENT_ERROR_INVALID_VALUE and the segment stays claimed. */
void *cellGcmSegmentBeginExternal(CellGcmSegment *segment);
int32_t cellGcmSegmentEndExternal(CellGcmSegment *segment, uint32_t usedBytes);

/* Emit, into `ring`, a CALL to the segment followed by its retirement
 * label.  The segment must be closed without overflow. */
int32_t cellGcmSegmentCall(CellGcmSegmentContext *ring, CellGcmSegment *segment);

/* Bytes the last End wrote (including the RETURN), and the words refused
 * since Begin. */
uint32_t cellGcmSegmentGetUsedBytes(const CellGcmSegment *segment);
uint32_t cellGcmSegmentGetOverflowWords(const CellGcmSegment *segment);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_GCM_SEGMENT_H */
