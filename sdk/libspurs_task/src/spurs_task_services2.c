/* SPU task services beyond the basic set: the "2" forms of yield, wait
 * signal and poll, receiving the workload flag, the task's volatile LS
 * area and the SPU GUID.
 *
 * The policy module's dispatcher (dispatch_base + 0xc4) takes a service
 * number: 1 yield, 2 wait signal, 3 poll, 4 receive workload flag, and
 * 17..20 their "2" forms.  A task about to block records what it waits
 * for at LS 0x2fd8 (8 = the workload flag).
 *
 * The workload flag lives in the CellSpurs instance: u32 flag at +0x6c
 * (0 = raised), u8 receiver workload at +0x77 (0xff = none).  A task
 * registers its taskset's workload as the receiver while it waits.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include "spurs_line.h"

#define SVC_RECEIVE_WORKLOAD_FLAG  4
#define SVC_YIELD2                 17
#define SVC_WAIT_SIGNAL2           18
#define SVC_TASK_POLL2             19
#define SVC_RECEIVE_WORKLOAD_FLAG2 20
#define WAIT_KIND_WORKLOAD_FLAG    8

#define PM_BUSY     0x8041080Au
#define CORE_INVAL  0x80410702u
#define CORE_ALIGN  0x80410710u
#define CORE_NULL   0x80410711u
#define LS_SIZE     0x40000u

extern int _cellSpursTaskCanCallBlockWait(void);
extern unsigned int cellSpursGetWorkloadId(void);

/* the SPU_LS_PARAM block (<stdlib.h>): heap size, stack size; weak so a
 * task without one links */
extern const uint32_t _cell_spu_ls_param[4] __attribute__((weak));
extern char _end[];

static uint32_t line[32] __attribute__((aligned(128)));

static inline uint32_t dispatch(int service)
{
	const uint32_t base = *(volatile uint32_t *)(uintptr_t)(0x2fb0 + 8);
	typedef uint32_t (*fn_t)(int);
	return ((fn_t)(uintptr_t)*(volatile uint32_t *)(uintptr_t)(base + 0xc4))(service);
}

int cellSpursYield2(void)
{
	int rc = _cellSpursTaskCanCallBlockWait();
	if (rc)
		return rc;
	return (int)dispatch(SVC_YIELD2);
}

int cellSpursWaitSignal2(void)
{
	int rc = _cellSpursTaskCanCallBlockWait();
	if (rc)
		return rc;
	*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
	rc = (int)dispatch(SVC_WAIT_SIGNAL2);
	*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
	return rc;
}

unsigned cellSpursTaskPoll2(void)
{
	return dispatch(SVC_TASK_POLL2);
}

/* Register (isSet) or unregister workload `wid` as the workload flag's
 * receiver.  The flag is reset either way. */
static int flag_receiver(unsigned wid, int isSet, int resetFlag)
{
	const uint64_t spurs = *(volatile uint64_t *)(uintptr_t)0x1c0;
	const unsigned expect = isSet ? 0xff : wid, want = isSet ? wid : 0xff;
	do {
		line_get_at(line, spurs);
		if (LINE_U8(line, 0x77) != expect)
			return (int)PM_BUSY;
		LINE_U8(line, 0x77) = (uint8_t)want;
		if (resetFlag)
			LINE_U32(line, 0x6c) = 0xffffffffu;
	} while (!line_put_at(line, spurs));
	return 0;
}

int _cellSpursWorkloadFlagReceiver(unsigned int wid, unsigned int isSet)
{
	return flag_receiver(wid, isSet, 1);
}

int _cellSpursWorkloadFlagReceiver2(unsigned int wid, unsigned int isSet)
{
	return flag_receiver(wid, isSet, 0);
}

static int receive_workload_flag(int service)
{
	const unsigned wid = cellSpursGetWorkloadId();
	int rc = _cellSpursWorkloadFlagReceiver2(wid, 1);
	if (rc)
		return rc;
	rc = _cellSpursTaskCanCallBlockWait();
	if (!rc) {
		*(volatile uint64_t *)TASK_WAIT_OBJECT = WAIT_KIND_WORKLOAD_FLAG;
		rc = (int)dispatch(service);
		*(volatile uint64_t *)TASK_WAIT_OBJECT = 0;
	}
	(void)_cellSpursWorkloadFlagReceiver2(wid, 0);
	return rc;
}

int cellSpursTaskReceiveWorkloadFlag(void)  { return receive_workload_flag(SVC_RECEIVE_WORKLOAD_FLAG); }
int cellSpursTaskReceiveWorkloadFlag2(void) { return receive_workload_flag(SVC_RECEIVE_WORKLOAD_FLAG2); }

/* The LS between the task's heap and its stack, which a context switch
 * does not have to preserve for the task. */
int cellSpursGetTaskVolatileArea(void **ptr, uint32_t *size)
{
	uint32_t start, top;
	if (*(volatile uint16_t *)(uintptr_t)0x1e8 != 0x544b)          /* 'TK' */
		return (int)TASK_PERM;
	if (!_cell_spu_ls_param || !_cell_spu_ls_param[1])
		return (int)TASK_STAT;
	start = _cell_spu_ls_param[0] + (uint32_t)(uintptr_t)_end;
	top = LS_SIZE - _cell_spu_ls_param[1];
	*ptr = (void *)(uintptr_t)start;
	if (start > top)
		return (int)TASK_INVAL;
	*size = top - start;
	return 0;
}

/* An SPU GUID is four "ila $2, imm18" instructions; bits 0-1 of each
 * immediate number the word, bits 2-17 carry 16 bits of the GUID. */
int cellSpursGetSpuGuid(const qword *pSpuGuid, uint64_t *guid)
{
	const vec_uint4 mask = spu_splats(0xfe0001ffu);
	const vec_uint4 form = { 0x42000002u, 0x42000082u, 0x42000102u, 0x42000182u };
	vec_uint4 w;
	uint64_t g = 0;
	unsigned i;
	if (!pSpuGuid || !guid)
		return (int)CORE_NULL;
	if (((uintptr_t)pSpuGuid & 0x7f) || ((uintptr_t)guid & 7))
		return (int)CORE_ALIGN;
	w = (vec_uint4)*pSpuGuid;
	if (spu_extract(spu_gather(spu_cmpeq(spu_and(w, mask), form)), 0) != 0xf)
		return (int)CORE_INVAL;
	for (i = 0; i < 4; ++i)
		g = (g << 16) | ((spu_extract(w, i) >> 9) & 0xffff);
	*guid = g;
	return 0;
}
