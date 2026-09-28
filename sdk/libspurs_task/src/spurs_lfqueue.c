/* SPU side of the SPURS lock-free queue.
 *
 * A CellSpursLFQueue is a libsync lock-free queue whose eaSignal names the
 * owning taskset (or, with bit 0 set, the SPURS instance for a queue bound
 * to the whole instance).  The libsync reservation and completion steps do
 * the work; this layer makes a blocking task sleep instead of spin and
 * wakes sleepers with task signals:
 *   - a waiter registers (workload id << 8) | task id and blocks in the
 *     wait-signal service, recording ea | 5 as the object it waits on;
 *   - End wakes each collected waiter through _cellSpursSendIWLSignal and
 *     a PPU sleeper through the queue's SPU event port.
 * libsync errors come back in the SPURS task facility (0x804109xx).
 * Independently written from the published object layout and protocol.
 */
#include <stdint.h>
#include <spu_mfcio.h>
#include <cell/sync/lfqueue.h>

#define ERR_INVAL        0x80410902u
#define ERR_STAT         0x8041090Fu
#define ERR_ALIGN        0x80410910u
#define ERR_NULL_POINTER 0x80410911u
#define ERR_FATAL        0x80410914u
#define SYNC_STAT        0x8041010Fu

/* LS 0x2fd8: the object a blocked task waits on, tagged with its kind */
#define TASK_WAIT_OBJECT   0x2fd8
#define WAIT_KIND_LFQUEUE  5

typedef int (*lfq_wait_fn)(uint64_t arg);
typedef uint32_t (*lfq_id_fn)(void);
typedef int (*lfq_signal_fn)(uint64_t eaSignal, uint32_t id);

/* libsync steps */
extern int _cellSyncLFQueueGetPushPointer(uint64_t ea, int *pPointer, lfq_wait_fn, lfq_id_fn);
extern int _cellSyncLFQueueGetPopPointer(uint64_t ea, int *pPointer, lfq_wait_fn, lfq_id_fn);
extern int _cellSyncLFQueueCompletePushPointer(uint64_t ea, int pointer, lfq_signal_fn,
                                               uint32_t allowThrow, uint32_t arg5);
extern int _cellSyncLFQueueCompletePopPointer(uint64_t ea, int pointer, lfq_signal_fn,
                                              uint32_t allowThrow, uint32_t noQueueFull);
extern int _cellSyncLFQueuePutTransfer(const void *ls, int pointer, unsigned int tag);
extern int _cellSyncLFQueueGetTransfer(void *ls, int pointer, unsigned int tag);

/* libspurs services */
extern uint64_t cellSpursGetTasksetAddress(void);
extern uint64_t cellSpursGetSpursAddress(void);
extern uint32_t _cellSpursGetIWLTaskId(void);
extern int cellSpursSendSignal(uint64_t eaTaskset, unsigned int idTask);
extern int cellSpursWaitSignal(void);
extern int _cellSpursTaskCanCallBlockWait(void);

/* a libsync status in the SPURS task facility */
static int task_status(int rc)
{
	return rc < 0 ? (int)(0x80410900u | ((unsigned)rc & 0xffu)) : rc;
}

int cellSpursLFQueueInitialize(uint64_t ea, uint64_t buffer, unsigned int size,
                               unsigned int depth, CellSyncQueueDirection direction)
{
	return task_status(cellSyncLFQueueInitialize(ea, buffer, size, depth, direction,
	                                             cellSpursGetTasksetAddress()));
}

int cellSpursLFQueueInitializeIWL(uint64_t ea, uint64_t buffer, unsigned int size,
                                  unsigned int depth, CellSyncQueueDirection direction)
{
	return task_status(cellSyncLFQueueInitialize(ea, buffer, size, depth, direction,
	                                             cellSpursGetSpursAddress() | 1));
}

int cellSpursLFQueueGetTasksetAddress(uint64_t ea, uint64_t *pEaTaskset)
{
	uint64_t signal;
	int rc;
	if (!pEaTaskset)
		return (int)ERR_NULL_POINTER;
	if ((rc = _cellSyncLFQueueGetSignalAddress(ea, &signal)) != 0)
		return task_status(rc);
	*pEaTaskset = (signal & 1) ? 0 : signal;
	return 0;
}

/* Wake task `id` ((workload << 8) | task) of the queue's taskset, or of
 * workload `id >> 8`'s taskset when eaSignal names a SPURS instance. */
int _cellSpursSendIWLSignal(uint64_t eaSignal, uint32_t id)
{
	uint64_t arg[2] __attribute__((aligned(16)));
	uint64_t spurs, info, taskset;
	unsigned wkl = id >> 8;
	if (!(eaSignal & 0xf))
		return cellSpursSendSignal(eaSignal, id & 0xff);
	if (wkl > 31)
		return (int)ERR_INVAL;
	/* CellSpurs workload info: 32-byte entries at 0xb00 (0-15) and 0x1000
	   (16-31); the argument at +0x08 is a taskset workload's taskset. */
	spurs = eaSignal & ~0xfull;
	info = spurs + ((wkl & 0x10) ? 0x1000u : 0xb00u) + (wkl & 0xfu) * 0x20u + 0x08u;
	mfc_get(&arg[(info >> 3) & 1], info, 8, 31, 0, 0);
	mfc_write_tag_mask(1u << 31);
	(void)mfc_read_tag_status_all();
	taskset = arg[(info >> 3) & 1];
	return cellSpursSendSignal(taskset, id & 0xff);
}

/* Block the task until a completer signals it */
static int task_wait(uint64_t object)
{
	int rc = _cellSpursTaskCanCallBlockWait();
	if (rc)
		return rc;
	*(volatile uint64_t *)TASK_WAIT_OBJECT = object;
	rc = cellSpursWaitSignal();
	*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
	return rc;
}

static int check_container(uint64_t ea, const void *c, const void *buffer, unsigned tag)
{
	if (!ea || !c || !buffer)
		return (int)ERR_NULL_POINTER;
	if (((uintptr_t)buffer & 15) || (ea & 0x7f))
		return (int)ERR_ALIGN;
	if (tag > 31)
		return (int)ERR_INVAL;
	return 0;
}

static int begin_status(int rc)
{
	return (unsigned)rc == SYNC_STAT ? (int)ERR_FATAL : task_status(rc);
}

static int end_status(int rc)
{
	if ((unsigned)rc == ERR_INVAL || (unsigned)rc == ERR_STAT)
		return (int)ERR_FATAL;       /* the wake itself failed */
	return task_status(rc);
}

int _cellSpursLFQueuePushBeginBody(uint64_t ea, CellSyncLFQueuePushContainer *c,
                                   unsigned int isBlocking)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	if (isBlocking && (rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;
	rc = _cellSyncLFQueueGetPushPointer(ea, &c->pointer, isBlocking ? task_wait : 0,
	                                    isBlocking ? _cellSpursGetIWLTaskId : 0);
	if (rc < 0)
		return begin_status(rc);
	return _cellSyncLFQueuePutTransfer(c->buffer, c->pointer, c->tag);
}

int _cellSpursLFQueuePopBeginBody(uint64_t ea, CellSyncLFQueuePopContainer *c,
                                  unsigned int isBlocking)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	if (isBlocking && (rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;
	rc = _cellSyncLFQueueGetPopPointer(ea, &c->pointer, isBlocking ? task_wait : 0,
	                                   isBlocking ? _cellSpursGetIWLTaskId : 0);
	if (rc < 0)
		return begin_status(rc);
	return _cellSyncLFQueueGetTransfer(c->buffer, c->pointer, c->tag);
}

int cellSpursLFQueuePushEnd(uint64_t ea, CellSyncLFQueuePushContainer *c)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	mfc_write_tag_mask(1u << c->tag);
	(void)mfc_read_tag_status_all();
	return end_status(_cellSyncLFQueueCompletePushPointer(ea, c->pointer,
	                                                     _cellSpursSendIWLSignal, 1, 0));
}

int cellSpursLFQueuePopEnd(uint64_t ea, CellSyncLFQueuePopContainer *c)
{
	int rc = check_container(ea, c, c ? c->buffer : 0, c ? c->tag : 0);
	if (rc)
		return rc;
	mfc_write_tag_mask(1u << c->tag);
	(void)mfc_read_tag_status_all();
	return end_status(_cellSyncLFQueueCompletePopPointer(ea, c->pointer,
	                                                    _cellSpursSendIWLSignal, 1, 0));
}
