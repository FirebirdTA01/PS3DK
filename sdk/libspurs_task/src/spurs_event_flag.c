/* SPU side of the SPURS event flag.
 *
 * A CellSpursEventFlag is one 128-byte line in main memory shared by PPU
 * threads and SPU tasks, so every update here is a getllar/putllc
 * read-modify-write of the whole line, retried until the reservation
 * holds.  Line layout (big-endian, offsets in bytes):
 *
 *   0x00 events                 u16  current event bits
 *   0x02 spuTaskPendingRecv     u16  bit i: the task in slot 15-i was woken
 *   0x04 ppuWaitMask            u16  bits a blocked PPU thread waits for
 *   0x06 ppuWaitSlotAndMode     u8   slot << 4 | wait mode of that thread
 *   0x07 ppuPendingRecv         u8   1 while the PPU waiter is being woken
 *   0x08 spuTaskUsedWaitSlots   u16  bit i: slot 15-i holds a waiting task
 *   0x0a spuTaskWaitMode        u16  bit i: slot 15-i waits in AND mode
 *   0x0c spuPort                u8   SPU event port to the PPU queue (0xff: none)
 *   0x0d isIwl                  u8   addr names a CellSpurs, not a taskset
 *   0x0e direction, 0x0f clearMode
 *   0x10 spuTaskWaitMask[16]    u16  per-slot wait masks
 *   0x30 pendingRecvTaskEvents[16] u16  the bits delivered to each woken waiter
 *   0x50 waitingTaskId[16], 0x60 waitingTaskWklId[16]  u8
 *   0x70 addr u64, 0x78 eventPortId u32, 0x7c eventQueueId u32
 *
 * Setting wakes a waiting PPU thread through the flag's SPU event port
 * (the queue cellSpursEventFlagAttachLv2EventQueue connected) and a waiting
 * SPU task by sending it a task signal; a waiting task blocks in
 * cellSpursWaitSignal.  Independently written from the published object
 * layout and wake protocol.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define ERR_AGAIN        0x80410901u
#define ERR_INVAL        0x80410902u
#define ERR_PERM         0x80410909u
#define ERR_BUSY         0x8041090Au
#define ERR_STAT         0x8041090Fu
#define ERR_ALIGN        0x80410910u
#define ERR_NULL_POINTER 0x80410911u
#define ERR_FATAL        0x80410914u

enum { MODE_OR = 0, MODE_AND = 1 };
enum { CLEAR_AUTO = 0, CLEAR_MANUAL = 1 };
enum { SPU2SPU = 0, SPU2PPU = 1, PPU2SPU = 2, ANY2ANY = 3 };
#define SLOTS        16
#define INVALID_PORT 0xff

typedef struct event_flag {
	uint16_t events;
	uint16_t spuTaskPendingRecv;
	uint16_t ppuWaitMask;
	uint8_t  ppuWaitSlotAndMode;
	uint8_t  ppuPendingRecv;
	uint16_t spuTaskUsedWaitSlots;
	uint16_t spuTaskWaitMode;
	uint8_t  spuPort;
	uint8_t  isIwl;
	uint8_t  direction;
	uint8_t  clearMode;
	uint16_t spuTaskWaitMask[SLOTS];
	uint16_t pendingRecvTaskEvents[SLOTS];
	uint8_t  waitingTaskId[SLOTS];
	uint8_t  waitingTaskWklId[SLOTS];
	uint64_t addr;
	uint32_t eventPortId;
	uint32_t eventQueueId;
} __attribute__((aligned(128))) event_flag_t;

_Static_assert(sizeof(event_flag_t) == 128, "event flag is one line");
_Static_assert(__builtin_offsetof(event_flag_t, spuTaskWaitMask) == 0x10, "wait masks");
_Static_assert(__builtin_offsetof(event_flag_t, pendingRecvTaskEvents) == 0x30, "pending events");
_Static_assert(__builtin_offsetof(event_flag_t, waitingTaskId) == 0x50, "task ids");
_Static_assert(__builtin_offsetof(event_flag_t, waitingTaskWklId) == 0x60, "workload ids");
_Static_assert(__builtin_offsetof(event_flag_t, addr) == 0x70, "addr");
_Static_assert(__builtin_offsetof(event_flag_t, eventQueueId) == 0x7c, "queue id");

/* libspurs.a services this file builds on */
extern unsigned int cellSpursGetTaskId(void);
extern uint64_t cellSpursGetTasksetAddress(void);
extern uint64_t cellSpursGetSpursAddress(void);
extern unsigned int cellSpursGetWorkloadId(void);
extern int cellSpursSendSignal(uint64_t eaTaskset, unsigned int idTask);
extern int cellSpursWaitSignal(void);

static event_flag_t line;

static void line_get(uint64_t ea)
{
	mfc_getllar(&line, ea, 0, 0);
	(void)mfc_read_atomic_status();
}

/* 1 when the conditional put kept the reservation */
static int line_put(uint64_t ea)
{
	mfc_putllc(&line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static unsigned slot_of_bit(unsigned bit) { return SLOTS - 1 - bit; }

/* Synchronous SPU-thread event to the PPU queue behind `port`
 * (the sys_spu_thread_send_event mailbox protocol). */
static uint32_t send_port_event(uint8_t port)
{
	if (port > 63)
		return 0x80010002u;
	if (spu_readchcnt(SPU_RdInMbox) != 0)
		return 0x8001000au;
	spu_writech(SPU_WrOutMbox, 0);
	spu_writech(SPU_WrOutIntrMbox, (uint32_t)port << 24);
	return spu_readch(SPU_RdInMbox);
}

/* The taskset a waiting task belongs to: the flag's own taskset, or for a
 * flag bound to a whole SPURS instance, workload wkl's argument, which for
 * a taskset workload is the taskset's address. */
static int waiter_taskset(uint64_t addr, int isIwl, unsigned wkl, uint64_t *taskset)
{
	uint64_t arg[2] __attribute__((aligned(16)));
	uint64_t info;
	if (!isIwl) {
		*taskset = addr;
		return 0;
	}
	if (wkl >= 32)
		return 1;
	/* CellSpurs workload info: 32-byte entries at 0xb00 (0-15) and 0x1000
	   (16-31); the 64-bit argument is at +0x08. */
	info = addr + ((wkl & 0x10) ? 0x1000u : 0xb00u) + (wkl & 0xfu) * 0x20u + 0x08u;
	mfc_get(&arg[(info >> 3) & 1], info, 8, 31, 0, 0);
	mfc_write_tag_mask(1u << 31);
	(void)mfc_read_tag_status_all();
	*taskset = arg[(info >> 3) & 1];
	return 0;
}

static int check_ea(uint64_t ea)
{
	if (!ea)
		return (int)ERR_NULL_POINTER;
	if (ea & 127)
		return (int)ERR_ALIGN;
	return 0;
}

int _cellSpursEventFlagInitialize(uint64_t ea, unsigned clearMode,
                                  unsigned direction, unsigned isIwl)
{
	int rc = check_ea(ea);
	unsigned i;
	if (rc)
		return rc;
	if (direction > ANY2ANY || clearMode > CLEAR_MANUAL)
		return (int)ERR_INVAL;
	for (i = 0; i < sizeof line; ++i)
		((volatile uint8_t *)&line)[i] = 0;
	line.direction = (uint8_t)direction;
	line.clearMode = (uint8_t)clearMode;
	line.spuPort = INVALID_PORT;
	line.isIwl = isIwl ? 1 : 0;
	line.addr = isIwl ? cellSpursGetSpursAddress() : cellSpursGetTasksetAddress();
	mfc_putlluc(&line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	return 0;
}

int cellSpursEventFlagSet(uint64_t ea, uint16_t bits)
{
	int rc = check_ea(ea);
	uint16_t pendingRecv, ppuEvents, delivered[SLOTS];
	uint8_t port = INVALID_PORT, isIwl = 0, taskId[SLOTS], wklId[SLOTS];
	uint64_t addr = 0;
	int wakePpu;
	unsigned i;
	if (rc)
		return rc;
	do {
		uint16_t used, toClear = 0;
		unsigned bit;
		line_get(ea);
		if (line.direction == PPU2SPU)
			return (int)ERR_PERM;   /* only the PPU sets this direction */
		wakePpu = 0;
		ppuEvents = 0;
		pendingRecv = 0;
		if ((line.direction == SPU2PPU || line.direction == ANY2ANY) && line.ppuWaitMask) {
			uint16_t relevant = (uint16_t)((line.events | bits) & line.ppuWaitMask);
			if ((line.ppuWaitMask & ~relevant) == 0
			    || ((line.ppuWaitSlotAndMode & 0x0f) == MODE_OR && relevant)) {
				line.ppuPendingRecv = 1;
				line.ppuWaitMask = 0;
				ppuEvents = relevant;
				toClear = relevant;
				line.pendingRecvTaskEvents[line.ppuWaitSlotAndMode >> 4] = relevant;
				wakePpu = 1;
			}
		}
		used = (uint16_t)(line.spuTaskUsedWaitSlots & ~line.spuTaskPendingRecv);
		for (bit = 0; bit < SLOTS; ++bit) {
			unsigned slot = slot_of_bit(bit);
			uint16_t mask, relevant;
			if (!(used & (1u << bit)))
				continue;
			mask = line.spuTaskWaitMask[slot];
			relevant = (uint16_t)((line.events | bits) & mask);
			if ((mask & ~relevant) == 0
			    || (((line.spuTaskWaitMode >> bit) & 1) == MODE_OR && relevant)) {
				toClear |= relevant;
				pendingRecv |= (uint16_t)(1u << bit);
				line.pendingRecvTaskEvents[slot] = relevant;
				delivered[slot] = relevant;
				taskId[slot] = line.waitingTaskId[slot];
				wklId[slot] = line.waitingTaskWklId[slot];
			}
		}
		line.events |= bits;
		line.spuTaskPendingRecv |= pendingRecv;
		if (line.clearMode == CLEAR_AUTO)
			line.events &= (uint16_t)~toClear;
		port = line.spuPort;
		isIwl = line.isIwl;
		addr = line.addr;
	} while (!line_put(ea));

	(void)ppuEvents;
	(void)delivered;
	if (wakePpu && send_port_event(port) != 0)
		return (int)ERR_FATAL;
	for (i = 0; i < SLOTS; ++i) {
		uint64_t taskset;
		if (!(pendingRecv & (1u << i)))
			continue;
		if (waiter_taskset(addr, isIwl, wklId[slot_of_bit(i)], &taskset))
			return (int)ERR_FATAL;
		rc = cellSpursSendSignal(taskset, taskId[slot_of_bit(i)]);
		if (rc)
			return (int)ERR_FATAL;
	}
	return 0;
}

int cellSpursEventFlagClear(uint64_t ea, uint16_t bits)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	do {
		line_get(ea);
		line.events &= (uint16_t)~bits;
	} while (!line_put(ea));
	return 0;
}

int _cellSpursEventFlagWait(uint64_t ea, uint16_t *bits, unsigned mode, unsigned block)
{
	int rc = check_ea(ea);
	uint16_t want, received = 0;
	unsigned bit = SLOTS, slot = 0;
	int waiting;
	if (rc)
		return rc;
	if (!bits)
		return (int)ERR_NULL_POINTER;
	if (mode > MODE_AND)
		return (int)ERR_INVAL;
	want = *bits;
	do {
		uint16_t relevant;
		line_get(ea);
		if (line.direction == SPU2PPU)
			return (int)ERR_PERM;   /* only the PPU waits in this direction */
		relevant = (uint16_t)(line.events & want);
		if (line.direction == ANY2ANY) {
			/* an AND waiter conflicts with any waiter whose mask overlaps but
			   differs; OR waiters only conflict with AND waiters */
			uint16_t others = (uint16_t)(line.spuTaskUsedWaitSlots & ~line.spuTaskPendingRecv);
			unsigned b;
			if (mode == MODE_OR)
				others &= line.spuTaskWaitMode;
			for (b = 0; b < SLOTS; ++b) {
				uint16_t m = line.spuTaskWaitMask[slot_of_bit(b)];
				if ((others & (1u << b)) && (m & want) && m != want)
					return (int)ERR_AGAIN;
			}
		}
		if ((want & ~relevant) == 0 || (mode == MODE_OR && relevant)) {
			if (line.clearMode == CLEAR_AUTO)
				line.events &= (uint16_t)~relevant;
			received = relevant;
			waiting = 0;
		} else {
			if (!block)
				return (int)ERR_BUSY;
			for (bit = 0; bit < SLOTS; ++bit)
				if (!(line.spuTaskUsedWaitSlots & (1u << bit)))
					break;
			if (bit == SLOTS)
				return (int)ERR_BUSY;
			slot = slot_of_bit(bit);
			line.spuTaskUsedWaitSlots |= (uint16_t)(1u << bit);
			line.spuTaskPendingRecv &= (uint16_t)~(1u << bit);
			if (mode == MODE_AND)
				line.spuTaskWaitMode |= (uint16_t)(1u << bit);
			else
				line.spuTaskWaitMode &= (uint16_t)~(1u << bit);
			line.spuTaskWaitMask[slot] = want;
			line.waitingTaskId[slot] = (uint8_t)cellSpursGetTaskId();
			line.waitingTaskWklId[slot] = (uint8_t)cellSpursGetWorkloadId();
			waiting = 1;
		}
	} while (!line_put(ea));

	if (waiting) {
		/* wait until a setter marks this slot delivered, then free it */
		for (;;) {
			int done;
			rc = cellSpursWaitSignal();
			if (rc)
				return rc;
			do {
				line_get(ea);
				done = (line.spuTaskPendingRecv >> bit) & 1;
				if (!done)
					break;
				received = line.pendingRecvTaskEvents[slot];
				line.spuTaskUsedWaitSlots &= (uint16_t)~(1u << bit);
				line.spuTaskPendingRecv &= (uint16_t)~(1u << bit);
			} while (!line_put(ea));
			if (done)
				break;
		}
	}
	*bits = received;
	return 0;
}

int cellSpursEventFlagGetDirection(uint64_t ea, unsigned *direction)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!direction)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*direction = line.direction;
	return 0;
}

int cellSpursEventFlagGetClearMode(uint64_t ea, unsigned *clearMode)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!clearMode)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*clearMode = line.clearMode;
	return 0;
}

int cellSpursEventFlagGetTasksetAddress(uint64_t ea, uint64_t *taskset)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!taskset)
		return (int)ERR_NULL_POINTER;
	line_get(ea);
	*taskset = line.isIwl ? 0 : line.addr;
	return 0;
}
