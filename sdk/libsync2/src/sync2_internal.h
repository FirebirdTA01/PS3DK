/* sync2_internal.h - shared layout and MFC helpers for the SPU libsync2.
 *
 * Every sync2 object is a 128-byte line in main memory, shared with the PPU
 * side (the libsync2 system module), so the layouts below are the wire
 * format both sides agree on.  All updates to a line go through a
 * reservation (GETLLAR / PUTLLC) and are retried until the conditional
 * store succeeds.
 *
 * Waiting queue header (at the start of the queue, inside the object line):
 *   +0x00 u16  bit 15 wake phase, bits 14..0 wake-ups in progress
 *   +0x02 u16  wake index
 *   +0x04 u16  bit 15 enter phase, bits 14..0 enters in progress
 *   +0x06 u16  enter index
 *   +0x08 u32  EA of the waiting buffer (8-byte entries)
 *   +0x0c u16  number of entries (maxWaiters + 1)
 * An index runs 0..entries-1; when it wraps, its phase bit flips, so
 * (phase, index) names a position modulo two laps.
 *
 * Waiting entry (8 bytes):
 *   word0 bit 31     lap parity the entry was written in (P)
 *         bit 30     equals P once the entry has been signalled (S)
 *         bit 29     a wake-up arrived before the waiter wrote the entry (E)
 *         bits 28..20 sequence tag, bumped as the entry is reused
 *         bits 19..16 log2 of the waiter's thread type id
 *         bits 15..0  receiver id bits 47..32
 *   word1            receiver id bits 31..0
 */
#ifndef SYNC2_INTERNAL_H
#define SYNC2_INTERNAL_H

#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/sync2/error.h>
#include <cell/sync2/thread_types.h>

#define S2_AGAIN        ((int)0x80410C01u)
#define S2_INVAL        ((int)0x80410C02u)
#define S2_NOMEM        ((int)0x80410C04u)
#define S2_DEADLK       ((int)0x80410C08u)
#define S2_PERM         ((int)0x80410C09u)
#define S2_BUSY         ((int)0x80410C0Au)
#define S2_STAT         ((int)0x80410C0Fu)
#define S2_ALIGN        ((int)0x80410C10u)
#define S2_NULL_POINTER ((int)0x80410C11u)
#define S2_NOT_SUPPORTED_THREAD ((int)0x80410C12u)
#define S2_NO_NOTIFIER  ((int)0x80410C13u)
#define S2_NO_SPU_CONTEXT_STORAGE ((int)0x80410C14u)

#define S2_PHASE        0x8000u
#define S2_COUNT        0x7fffu

#define S2_ENTRY_P      0x80000000u
#define S2_ENTRY_S      0x40000000u
#define S2_ENTRY_E      0x20000000u
#define S2_ENTRY_TAG    0x1ff00000u
#define S2_ENTRY_TLOG   0x000f0000u
#define S2_ENTRY_RECV   0x0000ffffu

typedef struct s2_queue_header {
	uint16_t wake_ctl;
	uint16_t wake_idx;
	uint16_t enter_ctl;
	uint16_t enter_idx;
	uint32_t buffer;
	uint16_t entries;
} s2_queue_header;

typedef struct s2_entry {
	uint32_t word0;
	uint32_t word1;
} s2_entry;

/* The 128-byte buffer every object operation reserves its line into. */
extern volatile uint8_t __sync2_line[128];

#define s2_barrier() __asm__ volatile("" ::: "memory")

static inline void s2_getllar(volatile void *ls, uint32_t ea)
{
	mfc_getllar(ls, ea, 0, 0);
	mfc_read_atomic_status();
	s2_barrier();
}

/* 1 when the conditional store went through. */
static inline int s2_putllc(volatile void *ls, uint32_t ea)
{
	s2_barrier();
	spu_dsync();
	mfc_putllc(ls, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static inline void s2_wait_tag(unsigned int tag)
{
	mfc_write_tag_mask(1u << tag);
	mfc_read_tag_status_all();
}

static inline void s2_get(volatile void *ls, uint32_t ea, uint32_t size, unsigned int tag)
{
	mfc_get(ls, ea, size, tag, 0, 0);
	s2_wait_tag(tag);
	s2_barrier();
}

static inline void s2_put(volatile void *ls, uint32_t ea, uint32_t size, unsigned int tag)
{
	s2_barrier();
	mfc_put(ls, ea, size, tag, 0, 0);
	s2_wait_tag(tag);
}

static inline __attribute__((noreturn)) void s2_halt(void)
{
	for (;;)
		__asm__ volatile("stopd $0,$0,$0");
}

typedef int (*s2_wait_fn)(CellSync2SignalReceiverId, CellSync2ObjectTypeId, uint64_t, uint64_t);

int __sync2_queue_enter(uint32_t eaQueue, uint16_t threadTypeId, uint64_t receiver,
                        s2_wait_fn waitSignal, CellSync2ObjectTypeId objectType,
                        uint64_t eaObject, uint64_t callbackArg, unsigned int dmaTag);
int __sync2_queue_wakeup(uint32_t eaQueue, CellSync2Notifier *const *notifiers,
                         unsigned int numNotifier, unsigned int dmaTag);

int __sync2_mutex_lock(uint32_t eaMutex, int wait, const CellSync2CallerThreadType *caller,
                       uint32_t count, unsigned int dmaTag);
int __sync2_mutex_unlock(uint32_t eaMutex, const CellSync2CallerThreadType *caller,
                         CellSync2Notifier *const *notifiers, unsigned int numNotifier,
                         uint32_t *countOut, unsigned int dmaTag);

int __sync2_semaphore_acquire(uint32_t eaSemaphore, int wait, unsigned int count,
                              const CellSync2CallerThreadType *caller, unsigned int dmaTag);
int __sync2_semaphore_release(uint32_t eaSemaphore, unsigned int count, const CellSync2CallerThreadType *caller,
                              CellSync2Notifier *const *notifiers, unsigned int numNotifier, unsigned int dmaTag);

int __sync2_cond_wait(uint32_t eaCond, const CellSync2CallerThreadType *caller, CellSync2Notifier *const *notifiers,
                      unsigned int numNotifier, unsigned int dmaTag);
int __sync2_cond_signal(uint32_t eaCond, int all, const CellSync2CallerThreadType *caller,
                        CellSync2Notifier *const *notifiers, unsigned int numNotifier, unsigned int dmaTag);

int __sync2_queue_push(uint32_t eaQueue, const void *data, const CellSync2CallerThreadType *caller,
                       CellSync2Notifier *const *notifiers, unsigned int numNotifier, int wait, unsigned int dmaTag);
int __sync2_queue_pop(uint32_t eaQueue, void *buffer, const CellSync2CallerThreadType *caller,
                      CellSync2Notifier *const *notifiers, unsigned int numNotifier, int wait, unsigned int dmaTag);

/* The argument checks every public entry point makes first. */
static inline int s2_check_args(uint64_t ea, const CellSync2ThreadConfig *config, unsigned int dmaTag)
{
	if (ea == 0 || config == 0)
		return S2_NULL_POINTER;
	if (ea & 0x7f)
		return S2_ALIGN;
	if (dmaTag >= 32)
		return S2_INVAL;
	return 0;
}

#endif
