/* SPU-side task creation: cellSpursCreateTask and the task attribute.
 *
 * CellSpursTaskset (main memory, first line):
 *   0x00 running, 0x10 ready, 0x20 pendingReady, 0x30 enabled,
 *   0x40 signalled, 0x50 waiting   128-bit task bitsets, task 0 = MSB
 *   0x74 wid u32                   the taskset's workload id
 * and from +0x80 one 0x30-byte record per task:
 *   +0x00 argument (16), +0x10 ELF EA u64,
 *   +0x18 context save area EA | allocated LS blocks u64,
 *   +0x20 LS pattern (16)
 * A task is created by claiming the first clear bit of `enabled`, writing
 * its record, then setting its `pendingReady` bit and signalling the
 * taskset's workload so a kernel picks it up.
 *
 * The task attribute is initialized and consumed on the SPU side only;
 * its layout (below) is private to this file.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/task.h>

#define TASK_AGAIN   0x80410901u
#define TASK_INVAL   0x80410902u
#define TASK_NOSYS   0x80410903u
#define TASK_ALIGN   0x80410910u
#define TASK_NULL    0x80410911u

#define MAX_TASKS        128
#define CONTEXT_MIN      1024           /* CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE */
#define TASK_INFO        0x80
#define TASK_INFO_SIZE   0x30

extern int _cellSpursSendWorkloadSignal(unsigned int wid);

static uint32_t line[32] __attribute__((aligned(128)));
static uint64_t info[6] __attribute__((aligned(16)));

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

static unsigned bits_set(vec_uint4 v)
{
	unsigned n = 0, i;
	for (i = 0; i < 4; ++i)
		n += (unsigned)__builtin_popcount(spu_extract(v, i));
	return n;
}

int cellSpursCreateTask(uint64_t eaTaskset, CellSpursTaskId *idTask, uint64_t eaElf,
                        uint64_t eaContext, uint32_t sizeContext, vec_uint4 lsPattern,
                        qword argument)
{
	unsigned id = MAX_TASKS, word, bit, wid;
	uint64_t allocBlocks = 0, record;
	if (!eaTaskset || !eaElf || !idTask)
		return (int)TASK_NULL;
	if ((eaTaskset & 0x7f) || (eaElf & 15) || (eaContext & 0x7f))
		return (int)TASK_ALIGN;
	if (eaContext) {
		if (sizeContext < CONTEXT_MIN)
			return (int)TASK_INVAL;
		allocBlocks = sizeContext > 0x3d400 ? 0x7a : (sizeContext - 0x400) >> 11;
		/* the pattern may not reach the SPURS area below the task top,
		   nor save more blocks than the area holds */
		if ((spu_extract(lsPattern, 0) & 0xfc000000u) || bits_set(lsPattern) > allocBlocks)
			return (int)TASK_INVAL;
	}

	/* claim a task id */
	do {
		line_get(eaTaskset);
		for (word = 0; word < 4; ++word) {
			uint32_t enabled = line[(0x30 >> 2) + word];
			if (enabled != 0xffffffffu) {
				bit = (unsigned)__builtin_clz(~enabled);
				id = word * 32 + bit;
				line[(0x30 >> 2) + word] = enabled | (0x80000000u >> bit);
				break;
			}
		}
		if (id >= MAX_TASKS)
			return (int)TASK_AGAIN;
	} while (!line_put(eaTaskset));
	wid = line[0x74 >> 2];

	/* its record */
	__builtin_memcpy(&info[0], &argument, 16);
	info[2] = eaElf;
	info[3] = eaContext | allocBlocks;
	if (!eaContext)
		lsPattern = (vec_uint4){ 0, 0, 0, 0 };
	__builtin_memcpy(&info[4], &lsPattern, 16);
	record = eaTaskset + TASK_INFO + (uint64_t)id * TASK_INFO_SIZE;
	mfc_putf(info, record, sizeof info, 31, 0, 0);
	mfc_write_tag_mask(1u << 31);
	(void)mfc_read_tag_status_all();

	/* make it ready */
	do {
		line_get(eaTaskset);
		line[(0x20 >> 2) + id / 32] |= 0x80000000u >> (id % 32);
	} while (!line_put(eaTaskset));
	*idTask = id;
	(void)_cellSpursSendWorkloadSignal(wid);
	return 0;
}

/* ---- task attribute ---------------------------------------------------- */

#define ATTR_MAGIC 0x54415452u          /* "TATR" */

typedef struct task_attr {
	uint32_t magic, revision;
	uint64_t eaElf;
	qword argument;
	uint64_t eaContext;
	uint32_t sizeContext, pad;
	vec_uint4 lsPattern;
	uint64_t eaExitCode;
} task_attr_t;

_Static_assert(sizeof(task_attr_t) <= sizeof(CellSpursTaskAttribute), "attribute fits");

int _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute *attr, unsigned int revision,
                                      unsigned int sdkVersion, uint64_t eaElf,
                                      const CellSpursTaskSaveConfig *saveConfig, qword argument)
{
	task_attr_t *a = (task_attr_t *)attr;
	unsigned i;
	(void)sdkVersion;
	if (!attr || !eaElf)
		return (int)TASK_NULL;
	if (((uintptr_t)attr & 15) || (eaElf & 15))
		return (int)TASK_ALIGN;
	for (i = 0; i < sizeof *attr; ++i)
		((volatile uint8_t *)attr)[i] = 0;
	a->magic = ATTR_MAGIC;
	a->revision = revision;
	a->eaElf = eaElf;
	a->argument = argument;
	if (saveConfig) {
		a->eaContext = saveConfig->eaContext;
		a->sizeContext = saveConfig->sizeContext;
		a->lsPattern = saveConfig->lsPattern;
	}
	return 0;
}

int cellSpursTaskAttributeSetExitCodeContainer(CellSpursTaskAttribute *attr, uint64_t eaExitCode)
{
	if (!attr)
		return (int)TASK_NULL;
	if ((uintptr_t)attr & 15)
		return (int)TASK_ALIGN;
	(void)eaExitCode;
	return (int)TASK_NOSYS;             /* exit-code containers: not yet */
}

int cellSpursCreateTaskWithAttribute(uint64_t eaTaskset, CellSpursTaskId *idTask,
                                     const CellSpursTaskAttribute *attr)
{
	const task_attr_t *a = (const task_attr_t *)attr;
	if (!attr)
		return (int)TASK_NULL;
	if ((uintptr_t)attr & 15)
		return (int)TASK_ALIGN;
	if (a->magic != ATTR_MAGIC)
		return (int)TASK_INVAL;
	return cellSpursCreateTask(eaTaskset, idTask, a->eaElf, a->eaContext, a->sizeContext,
	                           a->lsPattern, a->argument);
}
