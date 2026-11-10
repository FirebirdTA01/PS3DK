/*
 * PS3 Custom Toolchain — sdk/libgcm_cmd/src/gcm_segment.c
 *
 * Label-fenced command segments (<cell/gcm/gcm_segment.h>).  A segment
 * is written through an ordinary context set up with
 * cellGcmSetupContextData, so every cellGcm and rsx emitter works on it;
 * its callback refuses instead of wrapping.  Rules and measured
 * behaviour: docs/gcm-command-segments.md.
 */

#include <stdint.h>
#include <stddef.h>
#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/commands.h>

typedef gcmContextData CellGcmContextData;
typedef gcmContextCallback CellGcmContextCallback;
typedef struct _cellGcmSurface CellGcmSurface;
typedef gcmTransferScale CellGcmTransferScale;
typedef gcmTransferSurface CellGcmTransferSurface;
typedef gcmTransferSwizzle CellGcmTransferSwizzle;
#include <cell/gcm/gcm_command_link.h>
#include <cell/gcm/gcm_segment.h>

#ifdef PS3TC_GCM_SEGMENT_HOST_TEST
void ps3tc_gcm_segment_test_sleep(uint32_t microseconds);
#define SEGMENT_SLEEP(us) ps3tc_gcm_segment_test_sleep(us)
#else
#include <sys/timer.h>
#define SEGMENT_SLEEP(us) sys_timer_usleep(us)
#endif

/* RETURN: the one-word subroutine return (NV40 FIFO flag 0x00020000). */
#define SEGMENT_RETURN_WORD 0x00020000u

static volatile uint32_t *segment_label(const CellGcmSegment *s)
{
    return (volatile uint32_t *)gcmGetLabelAddress(s->labelIndex);
}

/* The segment context's callback: never wraps, never flushes.  It records
 * the words that did not fit and refuses, so the emitter writes nothing
 * for that reservation. */
static s32 segment_overflow(gcmContextData *context, u32 count)
{
    CellGcmSegment *s = (CellGcmSegment *)(void *)context;
    uint32_t sum = s->overflowWords + (uint32_t)count;
#ifdef SEGMENT_CONTROL_WRAP
    /* Host-test control only: behave like a ring callback and wrap. */
    context->current = context->begin;
    return 0;
#endif
    s->overflowWords = sum < s->overflowWords ? 0xffffffffu : sum;
    return -1;
}

int32_t cellGcmSegmentInit(CellGcmSegment *s, void *buffer, uint32_t sizeBytes,
                           uint8_t location, uint8_t labelIndex)
{
    uint32_t offset = 0;
    volatile uint32_t *label;

    if (!s || !buffer || sizeBytes < CELL_GCM_SEGMENT_MIN_SIZE ||
        (sizeBytes % CELL_GCM_SEGMENT_ALIGN) != 0 ||
        ((uintptr_t)buffer % CELL_GCM_SEGMENT_ALIGN) != 0 ||
        (location != GCM_LOCATION_RSX && location != GCM_LOCATION_CELL))
        return CELL_GCM_SEGMENT_ERROR_INVALID_VALUE;
    if (gcmAddressToOffset(buffer, &offset) != 0 ||
        offset >= CELL_GCM_SEGMENT_OFFSET_LIMIT ||
        sizeBytes > CELL_GCM_SEGMENT_OFFSET_LIMIT - offset)
        return CELL_GCM_SEGMENT_ERROR_BAD_ADDRESS;
    label = (volatile uint32_t *)gcmGetLabelAddress(labelIndex);
    if (!label)
        return CELL_GCM_SEGMENT_ERROR_BAD_ADDRESS;

    s->context.begin = s->context.end = s->context.current = NULL;
    s->context.callback = NULL;
    s->buffer = (uint32_t *)buffer;
    s->sizeBytes = sizeBytes;
    s->offset = offset;
    s->location = location;
    s->labelIndex = labelIndex;
    s->state = CELL_GCM_SEGMENT_STATE_IDLE;
    s->reserved = 0;
    s->serial = 0;
    s->usedBytes = 0;
    s->overflowWords = 0;
    s->calls = 0;
    *label = 0;
    __sync_synchronize();
    return CELL_OK;
}

int32_t cellGcmSegmentIsRetired(CellGcmSegment *s)
{
    if (!s) return 0;
    if (s->state == CELL_GCM_SEGMENT_STATE_IDLE) return 1;
    if (s->state != CELL_GCM_SEGMENT_STATE_SUBMITTED) return 0;
    if ((int32_t)(*segment_label(s) - s->serial) < 0) return 0;
    __sync_synchronize();   /* nothing after this reads ahead of the label */
    s->state = CELL_GCM_SEGMENT_STATE_IDLE;
    return 1;
}

int32_t cellGcmSegmentWaitRetired(CellGcmSegment *s, uint32_t maxPolls,
                                  uint32_t pollMicroseconds, uint32_t *pollsUsed)
{
    uint32_t polls = 0;
    int32_t rc = CELL_GCM_SEGMENT_ERROR_TIMEOUT;

    if (!s) return CELL_GCM_SEGMENT_ERROR_INVALID_VALUE;
    for (;;) {
        if (cellGcmSegmentIsRetired(s)) { rc = CELL_OK; break; }
        if (s->state != CELL_GCM_SEGMENT_STATE_SUBMITTED) {
            rc = CELL_GCM_SEGMENT_ERROR_STATE; break;
        }
        if (polls == maxPolls) break;          /* at most maxPolls sleeps */
        ++polls;
        SEGMENT_SLEEP(pollMicroseconds);
    }
    if (pollsUsed) *pollsUsed = polls;
    return rc;
}

CellGcmSegmentContext *cellGcmSegmentBegin(CellGcmSegment *s)
{
    /* A closed segment was never called and may be rewritten; an open or
     * in-flight one may not. */
    if (!s || s->state == CELL_GCM_SEGMENT_STATE_OPEN ||
        (s->state != CELL_GCM_SEGMENT_STATE_CLOSED && !cellGcmSegmentIsRetired(s)))
        return NULL;
    s->overflowWords = 0;
    s->usedBytes = 0;
    /* end = buffer + size - 4 bytes: the last word is kept for the RETURN. */
    cellGcmSetupContextData(&s->context, s->buffer, s->sizeBytes, segment_overflow);
    s->state = CELL_GCM_SEGMENT_STATE_OPEN;
    return &s->context;
}

int32_t cellGcmSegmentEnd(CellGcmSegment *s)
{
    if (!s || s->state != CELL_GCM_SEGMENT_STATE_OPEN || !s->context.begin)
        return CELL_GCM_SEGMENT_ERROR_STATE;     /* not open, or claimed externally */
    /* An emitter can only have advanced current up to end, which leaves the
     * reserved last word; anything else means the context was rewritten. */
    if (s->context.current < s->buffer || s->context.current > s->context.end ||
        s->context.end != s->buffer + (s->sizeBytes / 4u) - 1u) {
        s->overflowWords = s->overflowWords ? s->overflowWords : 1u;
        s->state = CELL_GCM_SEGMENT_STATE_CLOSED;
        return CELL_GCM_SEGMENT_ERROR_OVERFLOW;
    }
    *s->context.current++ = SEGMENT_RETURN_WORD;
    s->usedBytes = (uint32_t)((uintptr_t)s->context.current - (uintptr_t)s->buffer);
    s->state = CELL_GCM_SEGMENT_STATE_CLOSED;
    __sync_synchronize();   /* the words reach memory before any CALL */
    return s->overflowWords ? CELL_GCM_SEGMENT_ERROR_OVERFLOW : CELL_OK;
}

void *cellGcmSegmentBeginExternal(CellGcmSegment *s)
{
    if (!s || s->state == CELL_GCM_SEGMENT_STATE_OPEN ||
        (s->state != CELL_GCM_SEGMENT_STATE_CLOSED && !cellGcmSegmentIsRetired(s)))
        return NULL;
    s->context.begin = s->context.end = s->context.current = NULL;
    s->overflowWords = 0;
    s->usedBytes = 0;
    s->state = CELL_GCM_SEGMENT_STATE_OPEN;
    return s->buffer;
}

int32_t cellGcmSegmentEndExternal(CellGcmSegment *s, uint32_t usedBytes)
{
    if (!s || s->state != CELL_GCM_SEGMENT_STATE_OPEN || s->context.begin)
        return CELL_GCM_SEGMENT_ERROR_STATE;
    __sync_synchronize();   /* the writer's data is read after its completion */
    if (usedBytes < 4u || (usedBytes & 3u) || usedBytes > s->sizeBytes ||
        ((volatile uint32_t *)s->buffer)[usedBytes / 4u - 1u] != SEGMENT_RETURN_WORD)
        return CELL_GCM_SEGMENT_ERROR_INVALID_VALUE;
    s->usedBytes = usedBytes;
    s->state = CELL_GCM_SEGMENT_STATE_CLOSED;
    return CELL_OK;
}

int32_t cellGcmSegmentCall(CellGcmSegmentContext *ring, CellGcmSegment *s)
{
    uint32_t *before;

    if (!ring || !s) return CELL_GCM_SEGMENT_ERROR_INVALID_VALUE;
    if (s->state != CELL_GCM_SEGMENT_STATE_CLOSED) return CELL_GCM_SEGMENT_ERROR_STATE;
    if (s->overflowWords) return CELL_GCM_SEGMENT_ERROR_OVERFLOW;
    before = ring->current;
    rsxSetCallCommand(ring, s->offset);
    rsxSetWriteBackendLabel(ring, s->labelIndex, s->serial + 1u);
    /* Both commands fit unless the ring's own callback refused. */
    if (ring->current == before) return CELL_GCM_SEGMENT_ERROR_BUSY;
    s->serial += 1u;
    s->calls += 1u;
    s->state = CELL_GCM_SEGMENT_STATE_SUBMITTED;
    return CELL_OK;
}

uint32_t cellGcmSegmentGetUsedBytes(const CellGcmSegment *s)
{
    return s ? s->usedBytes : 0;
}

uint32_t cellGcmSegmentGetOverflowWords(const CellGcmSegment *s)
{
    return s ? s->overflowWords : 0;
}
