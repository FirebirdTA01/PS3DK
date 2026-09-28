/* SPU job-queue runtime, part 1: the job-side syscall layer, the job-queue
 * handles and information calls, WaitSignal, and the job-queue semaphore.
 *
 * The job-queue policy module runs a job with a syscall table: its LS
 * address sits in the low 28 bits of the job context's word at +0x14 (the
 * bits behind sizeJobDescriptor).  The runtime keeps the address, hides
 * it from the job again, and reaches the policy module through the
 * table: entry 0 suspends the job (WaitSignal), entry 1 is the DMA-wait
 * yield hook.
 *
 * CellSpursJobQueue fields used (main memory):
 *   0x060 u8   flags; bit 3 = closing
 *   0x06c u32  workload id (low 7 bits)
 *   0x070 u16  open handles (at most 1024)
 *   0x580      handle bitmap: 1024 bits, handle 0 in the MSB
 *   0xf04 u32  exit code of the failed job, 0xf0c cause
 *   0xf10 u32  SPURS instance EA, 0xf14 u32 max job descriptor size
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_context.h>
#include <cell/spurs/job_descriptor.h>

#define JOB_AGAIN  0x80410A01u
#define JOB_INVAL  0x80410A02u
#define JOB_PERM   0x80410A09u
#define JOB_BUSY   0x80410A0Au
#define JOB_STAT   0x80410A0Fu
#define JOB_ALIGN  0x80410A10u
#define JOB_NULL   0x80410A11u

#define MAX_HANDLES 1024
#define JQ_FLAGS    0x60
#define JQ_WID      0x6c
#define JQ_HANDLES  0x70
#define JQ_BITMAP   0x580
#define JQ_INFO     0xf00


/* ---- globals the public header and the job CRT refer to -------------- */

vec_uint4 (*_gfpCellSpursJobQueueYield)(uint32_t, uint32_t) __attribute__((aligned(16)));
extern unsigned int _gCellSpursJobQueueIsMemCheckEnabled;
extern unsigned int _gCellSpursJobQueueYieldHasAuxProc;
extern unsigned int _gCellSpursJobQueueIsTraceEnabled;

/* the policy module's syscall table (LS address) for the running job */
static uint32_t s_table;

/* the queue whose handles a job last looked at; Close forgets it */
uint64_t _jq_cached_ea;

static uint8_t line[128] __attribute__((aligned(128)));

static inline void line_get(uint64_t ea)
{
	mfc_getllar(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static inline int line_put(uint64_t ea)
{
	spu_dsync();
	mfc_putllc(line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

#define U8(o)  (*(volatile uint8_t *)(line + (o)))
#define U16(o) (*(volatile uint16_t *)(line + (o)))
#define U32(o) (*(volatile uint32_t *)(line + (o)))

static int check_ea(uint64_t ea)
{
	if (!ea)
		return (int)JOB_NULL;
	if (ea & 0x7f)
		return (int)JOB_ALIGN;
	return 0;
}

/* ---- syscall layer ------------------------------------------------------ */

int _spurs_jq_syscall_initialize(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
	volatile uint32_t *word14 = (volatile uint32_t *)((uint8_t *)ctx + 0x14);
	const uint32_t table = *word14 & 0x0fffffffu;
	(void)job;
	if (!table)
		return (int)JOB_NULL;
	s_table = table & 0x3ffff;
	_gfpCellSpursJobQueueYield = (vec_uint4 (*)(uint32_t, uint32_t))(uintptr_t)
		*(volatile uint32_t *)(uintptr_t)(s_table + 4);
	/* tracing and the memory check are not wired into this runtime */
	_gCellSpursJobQueueIsTraceEnabled = 0;
	_gCellSpursJobQueueIsMemCheckEnabled = 0;
	_gCellSpursJobQueueYieldHasAuxProc = 0;
	*word14 &= 0xf0000000u;                     /* hide the table again */
	return 0;
}

void _spurs_jq_syscall_finalize(CellSpursJobContext2 *ctx)
{
	(void)ctx;
}

/* ---- WaitSignal ---------------------------------------------------------- */

/* the running job binary's CRT signature ("JOBCRT Ver" + two version
   bytes) at _start + 0x20 */
extern const unsigned char _start[] __attribute__((weak));

static int wait_signal(uint64_t ea, unsigned int attr)
{
	static const char sig[10] = { 'J', 'O', 'B', 'C', 'R', 'T', ' ', 'V', 'e', 'r' };
	const volatile unsigned char *crt = (const volatile unsigned char *)_start + 0x20;
	typedef uint32_t (*suspend_t)(uint32_t);
	unsigned i;
	int version;
	if ((int)attr > 0) {
		if (!_start)
			return (int)JOB_PERM;
		for (i = 0; i < sizeof sig; ++i)
			if (crt[i] != (unsigned char)sig[i])
				return (int)JOB_PERM;
		version = (signed char)crt[10] * 16 + (signed char)crt[11] - 816;
		if ((unsigned)version <= 17)
			return (int)JOB_PERM;           /* a CRT too old to be suspended */
	}
	return (int)((suspend_t)(uintptr_t)*(volatile uint32_t *)(uintptr_t)s_table)((uint32_t)ea | attr);
}

int cellSpursJobQueueWaitSignal(uint64_t eaSuspendedJob)
{
	int rc = check_ea(eaSuspendedJob);
	return rc ? rc : wait_signal(eaSuspendedJob, 0);
}

int cellSpursJobQueueWaitSignal2(uint64_t eaSuspendedJob2, unsigned int attr)
{
	int rc = check_ea(eaSuspendedJob2);
	return rc ? rc : wait_signal(eaSuspendedJob2, attr);
}

/* ---- handles ------------------------------------------------------------- */

int cellSpursJobQueueOpen(uint64_t eaJobQueue, int *handle)
{
	unsigned q, bit;
	int found = -1, rc;
	if (!eaJobQueue || !handle)
		return (int)JOB_NULL;
	if (eaJobQueue & 0x7f)
		return (int)JOB_ALIGN;
	do {
		line_get(eaJobQueue);
		if (U8(JQ_FLAGS) & 8)
			return (int)JOB_STAT;           /* closing */
		if (U16(JQ_HANDLES) == MAX_HANDLES)
			return (int)JOB_AGAIN;
		U16(JQ_HANDLES) += 1;
	} while (!line_put(eaJobQueue));
	do {
		line_get(eaJobQueue + JQ_BITMAP);
		found = -1;
		for (q = 0; q < 32 && found < 0; ++q) {
			const uint32_t w = U32(q * 4);
			if (w != 0xffffffffu) {
				bit = (unsigned)__builtin_clz(~w);
				U32(q * 4) = w | (0x80000000u >> bit);
				found = (int)(q * 32 + bit);
			}
		}
	} while (!line_put(eaJobQueue + JQ_BITMAP));
	if (found < 0) {
		rc = (int)JOB_AGAIN;
		return rc;
	}
	*handle = found;
	return 0;
}

int cellSpursJobQueueClose(uint64_t eaJobQueue, int handle)
{
	const unsigned h = (unsigned)handle;
	const uint32_t bit = 0x80000000u >> (h % 32);
	if (!eaJobQueue)
		return (int)JOB_NULL;
	if (eaJobQueue & 0x7f)
		return (int)JOB_ALIGN;
	if (h >= MAX_HANDLES)
		return (int)JOB_INVAL;
	do {
		line_get(eaJobQueue + JQ_BITMAP);
		if (!(U32((h / 32) * 4) & bit))
			return (int)JOB_INVAL;          /* not open */
		U32((h / 32) * 4) &= ~bit;
	} while (!line_put(eaJobQueue + JQ_BITMAP));
	do {
		line_get(eaJobQueue);
		U16(JQ_HANDLES) -= 1;
	} while (!line_put(eaJobQueue));
	_jq_cached_ea = 0;
	return 0;
}

/* ---- information --------------------------------------------------------- */

uint64_t cellSpursJobQueueGetSpurs(uint64_t eaJobQueue)
{
	if (!eaJobQueue || (eaJobQueue & 0x7f))
		return 0;
	line_get(eaJobQueue + JQ_INFO);
	return U32(0x10);
}

int cellSpursJobQueueGetHandleCount(uint64_t eaJobQueue)
{
	int rc = check_ea(eaJobQueue);
	if (rc)
		return rc;
	line_get(eaJobQueue);
	return U16(JQ_HANDLES);
}

int cellSpursJobQueueGetError(uint64_t eaJobQueue, int *exitCode, void **cause)
{
	if (!eaJobQueue || !exitCode || !cause)
		return (int)JOB_NULL;
	if (eaJobQueue & 0x7f)
		return (int)JOB_ALIGN;
	line_get(eaJobQueue + JQ_INFO);
	*exitCode = (int)U32(0x04);
	*cause = *exitCode ? (void *)(uintptr_t)U32(0x0c) : 0;
	return 0;
}

int cellSpursJobQueueGetMaxSizeJobDescriptor(uint64_t eaJobQueue)
{
	int rc = check_ea(eaJobQueue);
	if (rc)
		return rc;
	line_get(eaJobQueue + JQ_INFO);
	return (int)U32(0x14);
}

int cellSpursGetJobQueueId(uint64_t eaJobQueue, unsigned int *pId)
{
	unsigned wid;
	if (!eaJobQueue || !pId)
		return (int)JOB_NULL;
	if (eaJobQueue & 0x7f)
		return (int)JOB_ALIGN;
	line_get(eaJobQueue);
	wid = U32(JQ_WID) & 0x7f;
	if (wid > 31)
		return (int)JOB_STAT;
	*pId = wid;
	return 0;
}
