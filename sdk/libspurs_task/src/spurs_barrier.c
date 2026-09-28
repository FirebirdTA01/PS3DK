/* SPU side of the SPURS barrier.
 *
 * A CellSpursBarrier is one 128-byte line in main memory.  The PPU side
 * initializes it (taskset form) by zeroing it and setting:
 *   0x04 remained  u32  tasks that still have to notify
 *   0x34 taskset   u32  EA of the owning taskset
 * Notify and wait happen only in SPU tasks, which also use:
 *   0x00 released  u32  nonzero once every task has notified
 *   0x08 waiting   128-bit set of task ids blocked in wait (task 0 = MSB)
 * The last notifier marks the barrier released and signals every waiting
 * task; a waiter registers its id and blocks in cellSpursWaitSignal until
 * the barrier is released.
 * Independently written from the published object layout and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define TASK_INVAL   0x80410902u
#define TASK_BUSY    0x8041090Au
#define TASK_STAT    0x8041090Fu
#define TASK_ALIGN   0x80410910u
#define TASK_NULL    0x80410911u

#define MAX_TASKS 128

extern unsigned int cellSpursGetTaskId(void);
extern uint64_t cellSpursGetTasksetAddress(void);
extern int cellSpursSendSignal(uint64_t eaTaskset, unsigned int idTask);
extern int cellSpursWaitSignal(void);
extern int _cellSpursTaskCanCallBlockWait(void);

/* LS 0x2fd8: the object a blocked task waits on, tagged with its kind */
#define TASK_WAIT_OBJECT 0x2fd8
#define WAIT_KIND_BARRIER 2

static uint32_t line[32] __attribute__((aligned(128)));

enum { RELEASED = 0x00 / 4, REMAINED = 0x04 / 4, WAITING = 0x08 / 4, TASKSET = 0x34 / 4 };

static void line_get(uint64_t ea)
{
	mfc_getllar(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static int line_put(uint64_t ea)
{
	spu_dsync();
	mfc_putllc(line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static int check_ea(uint64_t ea)
{
	if (!ea)
		return (int)TASK_NULL;
	if (ea & 0x7f)
		return (int)TASK_ALIGN;
	return 0;
}

int cellSpursBarrierInitialize(uint64_t ea, unsigned int total)
{
	unsigned i;
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!total || total > MAX_TASKS)
		return (int)TASK_INVAL;
	for (i = 0; i < 32; ++i)
		line[i] = 0;
	line[REMAINED] = total;
	line[TASKSET] = (uint32_t)cellSpursGetTasksetAddress();
	mfc_putlluc(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	return 0;
}

int _cellSpursBarrierNotify(uint64_t ea, unsigned isBlocking)
{
	uint32_t waiting[4] = { 0, 0, 0, 0 };
	uint64_t taskset;
	unsigned i, released;
	int rc = check_ea(ea);
	(void)isBlocking;                   /* notifying never has to wait */
	if (rc)
		return rc;
	do {
		line_get(ea);
		if (line[RELEASED] || !line[REMAINED])
			return (int)TASK_STAT;      /* every task already notified */
		released = --line[REMAINED] == 0;
		if (released) {
			line[RELEASED] = 1;
			for (i = 0; i < 4; ++i) {
				waiting[i] = line[WAITING + i];
				line[WAITING + i] = 0;
			}
		}
	} while (!line_put(ea));

	if (released) {
		taskset = line[TASKSET];
		for (i = 0; i < MAX_TASKS; ++i)
			if (waiting[i / 32] & (0x80000000u >> (i % 32)))
				if (cellSpursSendSignal(taskset, i))
					return (int)TASK_STAT;
	}
	return 0;
}

int _cellSpursBarrierWait(uint64_t ea, unsigned isBlocking)
{
	unsigned id = cellSpursGetTaskId();
	int rc = check_ea(ea), registered;
	if (rc)
		return rc;
	if (isBlocking && (rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;
	for (;;) {
		do {
			line_get(ea);
			if (line[RELEASED])
				return 0;
			if (!isBlocking)
				return (int)TASK_BUSY;
			line[WAITING + id / 32] |= 0x80000000u >> (id % 32);
			registered = 1;
		} while (!line_put(ea));
		if (registered) {
			*(volatile uint64_t *)TASK_WAIT_OBJECT = ea | WAIT_KIND_BARRIER;
			rc = cellSpursWaitSignal();
			*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
			if (rc)
				return rc;
		}
	}
}

int cellSpursBarrierGetTasksetAddress(uint64_t ea, uint64_t *pEaTaskset)
{
	int rc = check_ea(ea);
	if (rc)
		return rc;
	if (!pEaTaskset)
		return (int)TASK_NULL;
	line_get(ea);
	*pEaTaskset = line[TASKSET];
	return 0;
}

/* entry points of the older API: blocking and try forms as functions */
int cellSpursBarrierNotify(uint64_t ea)    { return _cellSpursBarrierNotify(ea, 1); }
int cellSpursBarrierTryNotify(uint64_t ea) { return _cellSpursBarrierNotify(ea, 0); }
int cellSpursBarrierWait(uint64_t ea)      { return _cellSpursBarrierWait(ea, 1); }
int cellSpursBarrierTryWait(uint64_t ea)   { return _cellSpursBarrierWait(ea, 0); }
