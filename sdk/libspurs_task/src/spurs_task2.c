/* SPU side of the Taskset2 task forms: cellSpursCreateTask2 (and
 * WithBinInfo), cellSpursJoinTask2 / cellSpursTryJoinTask2.
 *
 * A CellSpursTaskset2 is a CellSpursTaskset (bitsets and 0x30-byte task
 * records, see spurs_create_task.c) extended with:
 *   0x0070 u8    flags; bit 1 = shutting down (no new tasks)
 *   0x0071 u8    live Task2 tasks
 *   0x1890 u32   size of the object (a Taskset2 is at least 0x2900)
 *   0x1894 u32   1 = the taskset delivers no exit codes
 *   0x1898/0x189c  event flag ids; one is set for a Taskset2
 *   0x18a0 u32   event flag set (bit 1) when the last task of a shutting
 *                down taskset is joined
 *   0x18a8 u64   EA of the task name buffer (32 bytes per task), or 0
 *   0x1980       one 16-byte exit record per task:
 *                  +0x00 u64  joining task: its taskset EA | task id
 *                  +0x08 u32  exit code
 *                  +0x0e u8   waiter kind (2 = an SPU task)
 *                  +0x0f u8   state: low nibble 1 running, 2 exited with a
 *                             code, 3 exited without one, 4 joined;
 *                             0x10 = the creating call has finished
 * A Task2 task's record marks its ELF EA with bit 2; the policy module
 * fills in the exit record when the task exits and wakes a joiner.
 * Joining releases the task id.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/event_flag.h>
#include <cell/spurs/task.h>
#include "spurs_line.h"

#define MAX_TASKS        128
#define CONTEXT_MIN      1024
#define TASK_RECORD      0x80
#define TASK_RECORD_SIZE 0x30
#define TS2_INFO         0x1880
#define TS2_SIZE_MIN     0x2900
#define EXIT_RECORDS     0x1980
#define NAME_SIZE        32
#define ELF_TASK2        4
#define WAIT_KIND_JOIN2  7

#define X_RUNNING   1
#define X_EXITED    2
#define X_NO_CODE   3
#define X_JOINED    4
#define X_CREATED   0x10

extern unsigned int cellSpursGetTaskId(void);
extern uint64_t cellSpursGetTasksetAddress(void);
extern int cellSpursWaitSignal(void);
extern int _cellSpursTaskCanCallBlockWait(void);
extern int _cellSpursSendWorkloadSignal(unsigned int wid);

static uint32_t line[32] __attribute__((aligned(128)));
static uint64_t record[6] __attribute__((aligned(16)));
static uint8_t head[128] __attribute__((aligned(128)));
static char name[NAME_SIZE] __attribute__((aligned(16)));

static unsigned bits_set(vec_uint4 v)
{
	unsigned n = 0, i;
	for (i = 0; i < 4; ++i)
		n += (unsigned)__builtin_popcount(spu_extract(v, i));
	return n;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void _cellSpursTaskAttribute2Initialize(CellSpursTaskAttribute2 *attr, uint32_t revision)
{
	unsigned i;
	for (i = 0; i < sizeof *attr; ++i)
		((volatile uint8_t *)attr)[i] = 0;
	attr->revision = revision;
}

/* a 32-bit big-endian SPU ELF whose loadable segments stay above the
 * SPURS area (only the program headers inside the first 128 bytes are
 * checked) */
static int check_elf(uint64_t eaElf, unsigned tag)
{
	static const uint8_t ident[16] = { 0x7f, 'E', 'L', 'F', 1, 2, 1 };
	unsigned i, n, step, ph;
	dma_get_wait(head, eaElf, sizeof head, tag);
	for (i = 0; i < 16; ++i)
		if (head[i] != ident[i])
			return (int)TASK_NOEXEC;
	if (((head[0x12] << 8) | head[0x13]) != 23 || be32(head + 0x18) < 0x3000)
		return (int)TASK_NOEXEC;
	ph = be32(head + 0x1c);
	step = (head[0x2a] << 8) | head[0x2b];
	n = (head[0x2c] << 8) | head[0x2d];
	for (i = 0; i < n && ph + 12 <= sizeof head; ++i, ph += step)
		if (be32(head + ph) == 1 /* PT_LOAD */ && be32(head + ph + 8) < 0x3000)
			return (int)TASK_NOEXEC;
	return 0;
}

int cellSpursCreateTask2(uint64_t eaTaskset, CellSpursTaskId *idTask, uint64_t eaElf,
                         qword argument, const CellSpursTaskAttribute2 *attr)
{
	CellSpursTaskAttribute2 local;
	const unsigned tag = spurs_kernel_tag();
	unsigned id = MAX_TASKS, word, bit, wid;
	uint64_t allocBlocks = 0, exitRecord, nameBuffer;
	vec_uint4 pattern = { 0, 0, 0, 0 };
	int rc;
	if (!eaTaskset)
		return (int)TASK_NULL;
	if (eaTaskset & 0x7f)
		return (int)TASK_ALIGN;
	if (!attr) {
		_cellSpursTaskAttribute2Initialize(&local, 0);
		attr = &local;
	}
	line_get_at(line, eaTaskset + TS2_INFO);
	if (LINE_U32(line, 0x10) < TS2_SIZE_MIN || (!LINE_U32(line, 0x18) && !LINE_U32(line, 0x1c)))
		return (int)TASK_PERM;                  /* not a Taskset2 */
	if (!eaElf)
		return (int)TASK_NULL;
	if ((eaElf & 15) || (attr->eaContext & 0x7f))
		return (int)TASK_ALIGN;
	if ((rc = check_elf(eaElf, tag)) != 0)
		return rc;
	if (attr->eaContext) {
		const uint32_t size = attr->sizeContext;
		const vec_uint4 p = *(const vec_uint4 *)&attr->lsPattern;
		if (size < CONTEXT_MIN)
			return (int)TASK_INVAL;
		allocBlocks = size > 0x3d400 ? 0x7a : (size - 0x400) >> 11;
		if ((spu_extract(p, 0) & 0xfc000000u) || bits_set(p) > allocBlocks)
			return (int)TASK_INVAL;
		pattern = p;
	}

	/* claim a task id */
	do {
		line_get_at(line, eaTaskset);
		rc = 0;
		if (LINE_U8(line, 0x70) & 2) {
			rc = (int)TASK_BUSY;
			continue;
		}
		for (word = 0; word < 4; ++word)
			if (line[(0x30 >> 2) + word] != 0xffffffffu)
				break;
		if (word == 4) {
			rc = (int)TASK_AGAIN;
			continue;
		}
		bit = (unsigned)__builtin_clz(~line[(0x30 >> 2) + word]);
		id = word * 32 + bit;
		line[(0x00 >> 2) + word] &= ~(0x80000000u >> bit);
		line[(0x10 >> 2) + word] &= ~(0x80000000u >> bit);
		line[(0x20 >> 2) + word] &= ~(0x80000000u >> bit);
		line[(0x30 >> 2) + word] |= 0x80000000u >> bit;
		line[(0x40 >> 2) + word] &= ~(0x80000000u >> bit);
		line[(0x50 >> 2) + word] &= ~(0x80000000u >> bit);
		LINE_U8(line, 0x71) += 1;
	} while (!line_put_at(line, eaTaskset));
	if (rc)
		return rc;
	wid = line[0x74 >> 2];

	/* its task record */
	__builtin_memcpy(&record[0], &argument, 16);
	record[2] = eaElf | ELF_TASK2;
	record[3] = attr->eaContext | allocBlocks;
	__builtin_memcpy(&record[4], &pattern, 16);
	dma_put_wait(record, eaTaskset + TASK_RECORD + (uint64_t)id * TASK_RECORD_SIZE, sizeof record, tag);

	/* its exit record: running */
	exitRecord = eaTaskset + EXIT_RECORDS + (uint64_t)id * 16;
	record[0] = 0;
	record[1] = X_RUNNING;
	dma_put_wait(record, exitRecord, 16, tag);

	/* make it ready */
	do {
		line_get_at(line, eaTaskset);
		line[(0x20 >> 2) + id / 32] |= 0x80000000u >> (id % 32);
	} while (!line_put_at(line, eaTaskset));
	(void)_cellSpursSendWorkloadSignal(wid);

	/* its name */
	line_get_at(line, eaTaskset + TS2_INFO);
	nameBuffer = LINE_U64(line, 0x28);
	if (attr->name && nameBuffer) {
		unsigned i;
		for (i = 0; i < NAME_SIZE; ++i)
			name[i] = attr->name[i];
		name[NAME_SIZE - 1] = 0;
		dma_put_wait(name, nameBuffer + (uint64_t)id * NAME_SIZE, NAME_SIZE, tag);
	}

	/* creation finished: a taskset without exit codes marks it now */
	{
		const int noCode = LINE_U32(line, 0x14) == 1;
		const uint64_t lineEa = exitRecord & ~0x7full;
		const unsigned off = (unsigned)(exitRecord & 0x7f) + 15;
		do {
			line_get_at(line, lineEa);
			if (noCode && LINE_U8(line, off) != X_EXITED)
				LINE_U8(line, off) = X_NO_CODE;
			LINE_U8(line, off) |= X_CREATED;
		} while (!line_put_at(line, lineEa));
	}
	*idTask = id;
	return 0;
}

int cellSpursCreateTask2WithBinInfo(uint64_t eaTaskset, CellSpursTaskId *idTask,
                                    uint64_t eaTaskBinInfo, qword argument,
                                    uint64_t eaContext, const char *taskName, void *reserved)
{
	static CellSpursTaskBinInfo info;
	CellSpursTaskAttribute2 attr;
	if (!eaTaskBinInfo)
		return (int)TASK_NULL;
	if (eaTaskBinInfo & 15)
		return (int)TASK_ALIGN;
	if (reserved)
		return (int)TASK_INVAL;
	dma_get_wait(&info, eaTaskBinInfo, sizeof info, spurs_kernel_tag());
	if (info.__reserved__)
		return (int)TASK_INVAL;
	_cellSpursTaskAttribute2Initialize(&attr, 0);
	attr.sizeContext = info.sizeContext;
	attr.eaContext = eaContext;
	attr.lsPattern = info.lsPattern;
	attr.name = taskName;
	return cellSpursCreateTask2(eaTaskset, idTask, info.eaElf, argument, &attr);
}

/* ---- join ------------------------------------------------------------------ */

static int join_task2(uint64_t eaTaskset, CellSpursTaskId idTask, int *exitCode, int isBlocking)
{
	const unsigned tag = spurs_kernel_tag();
	uint64_t exitRecord, lineEa, nameBuffer;
	unsigned off, state, lastOfShutdown = 0;
	int rc, wait = 0;
	if (!eaTaskset || !exitCode)
		return (int)TASK_NULL;
	if (eaTaskset & 0x7f)
		return (int)TASK_ALIGN;
	if (idTask >= MAX_TASKS)
		return (int)TASK_INVAL;
	exitRecord = eaTaskset + EXIT_RECORDS + (uint64_t)idTask * 16;
	lineEa = exitRecord & ~0x7full;
	off = (unsigned)(exitRecord & 0x7f);
	if (isBlocking && (rc = _cellSpursTaskCanCallBlockWait()) != 0)
		return rc;

	do {
		line_get_at(line, lineEa);
		state = LINE_U8(line, off + 15);
		rc = (int)TASK_STAT;
		wait = 0;
		if (LINE_U8(line, off + 14))
			;                                   /* someone joins it already */
		else if ((state & 15) == X_EXITED) {
			*exitCode = (int)LINE_U32(line, off + 8);
			LINE_U8(line, off + 15) = X_JOINED;
			rc = 0;
		} else if ((state & 15) == X_NO_CODE) {
			LINE_U8(line, off + 15) = X_JOINED;
			rc = (int)TASK_SHUTDOWN;
		} else if ((state & 15) == X_RUNNING && (state & X_CREATED)) {
			if (!isBlocking)
				rc = (int)TASK_AGAIN;
			else if (LINE_U64(line, off))
				rc = (int)TASK_BUSY;
			else {
				LINE_U8(line, off + 14) = 2;    /* an SPU task joins */
				LINE_U64(line, off) = cellSpursGetTasksetAddress() | cellSpursGetTaskId();
				rc = 0;
				wait = 1;
			}
		}
	} while (!line_put_at(line, lineEa));
	if (rc)
		return rc;

	if (wait) {
		if (_cellSpursTaskCanCallBlockWait())
			return (int)TASK_FATAL;
		*(volatile uint64_t *)TASK_WAIT_OBJECT = exitRecord | WAIT_KIND_JOIN2;
		rc = cellSpursWaitSignal();
		*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
		if (rc)
			return (int)TASK_FATAL;
		do {
			line_get_at(line, lineEa);
			state = LINE_U8(line, off + 15) & 15;
			rc = (int)TASK_STAT;
			if (state == X_EXITED) {
				*exitCode = (int)LINE_U32(line, off + 8);
				LINE_U8(line, off + 15) = X_JOINED;
				rc = 0;
			} else if (state == X_NO_CODE) {
				LINE_U8(line, off + 15) = X_JOINED;
				rc = (int)TASK_SHUTDOWN;
			}
		} while (!line_put_at(line, lineEa));
		if (rc)
			return rc;
	}

	/* release the task id */
	record[0] = record[1] = 0;
	dma_put_wait(record, exitRecord, 16, tag);
	do {
		const unsigned word = idTask / 32;
		const uint32_t bit = 0x80000000u >> (idTask % 32);
		line_get_at(line, eaTaskset);
		line[(0x00 >> 2) + word] &= ~bit;
		line[(0x30 >> 2) + word] &= ~bit;
		line[(0x40 >> 2) + word] &= ~bit;
		LINE_U8(line, 0x71) -= 1;
		lastOfShutdown = LINE_U8(line, 0x71) == 0 && (LINE_U8(line, 0x70) & 2);
	} while (!line_put_at(line, eaTaskset));

	line_get_at(line, eaTaskset + TS2_INFO);
	if (lastOfShutdown)
		(void)sys_event_flag_set_bit_impatient(LINE_U32(line, 0x20), 1);
	nameBuffer = LINE_U64(line, 0x28);
	if (nameBuffer) {
		unsigned i;
		for (i = 0; i < NAME_SIZE; ++i)
			name[i] = 0;
		dma_put_wait(name, nameBuffer + (uint64_t)idTask * NAME_SIZE, NAME_SIZE, tag);
	}
	return 0;
}

int cellSpursJoinTask2(uint64_t eaTaskset, CellSpursTaskId idTask, int *exitCode)
{
	return join_task2(eaTaskset, idTask, exitCode, 1);
}

int cellSpursTryJoinTask2(uint64_t eaTaskset, CellSpursTaskId idTask, int *exitCode)
{
	return join_task2(eaTaskset, idTask, exitCode, 0);
}
