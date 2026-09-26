/* mutex_ticket.h -- CellSyncMutex word transitions shared by the SPU code
 * and its host test (tests/sdk/libsync-spu-host-test.sh).
 *
 * CellSyncMutex is one 32-bit word, the same object on PPU and SPU: two
 * big-endian 16-bit counters, current_ticket at byte 0 and next_ticket at
 * byte 2.  Read as a big-endian word, current is the high half.
 *
 *   Lock:    take a ticket (the old next_ticket) and increment next_ticket,
 *            then wait until current_ticket equals the ticket.
 *   TryLock: only when current == next (free and nobody queued), take the
 *            ticket as above; otherwise BUSY without changing the word.
 *   Unlock:  increment current_ticket, admitting the next ticket holder.
 *
 * Both counters wrap at 16 bits.  These helpers are the pure word
 * arithmetic; mutex.c applies them under an MFC line reservation.
 */
#ifndef PS3DK_LIBSYNC_MUTEX_TICKET_H
#define PS3DK_LIBSYNC_MUTEX_TICKET_H

#include <stdint.h>

static inline uint16_t __sync_mutex_current(uint32_t word)
{
    return (uint16_t)(word >> 16);
}

static inline uint16_t __sync_mutex_next(uint32_t word)
{
    return (uint16_t)word;
}

/* The word after taking a ticket (next_ticket + 1). */
static inline uint32_t __sync_mutex_take_ticket(uint32_t word)
{
    return (word & 0xffff0000u) | (uint16_t)(__sync_mutex_next(word) + 1u);
}

/* The word after a release (current_ticket + 1). */
static inline uint32_t __sync_mutex_release(uint32_t word)
{
    return ((uint32_t)(uint16_t)(__sync_mutex_current(word) + 1u) << 16)
           | __sync_mutex_next(word);
}

static inline int __sync_mutex_is_free(uint32_t word)
{
    return __sync_mutex_current(word) == __sync_mutex_next(word);
}

#endif /* PS3DK_LIBSYNC_MUTEX_TICKET_H */
