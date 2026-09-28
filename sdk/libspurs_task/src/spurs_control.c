/* SPU-side SPURS workload control: shut down a workload, a taskset or a
 * job chain, run or kick a job chain, and the job guard.
 *
 * Everything here is an atomic update of one 128-byte line of a shared
 * object in main memory (getllar/putllc, retried when the reservation is
 * lost).  CellSpurs lines used (offsets from the instance):
 *
 *   0x00  wklReadyCount1[16] u8, wklIdleSpuCountOrReadyCount2[16] u8 (0x10)
 *   0x72  sysSrvMessage u8          nonzero: the system service has news
 *   0x80  wklState1[16] u8, wklStatus1[16] u8 (0x90, SPUs running each
 *         workload), wklEvent1[16] u8 (0xa0), wklEnabled u32 (0xb0),
 *         sysSrvMsgUpdateWorkload u8 (0xbd), wklState2/Status2/Event2 for
 *         workloads 16..31 at 0xd0/0xe0/0xf0
 *
 * A taskset and a job chain both keep their workload id at +0x74; a job
 * chain keeps autoReadyCount at +0x24 and its job-manager revision at
 * +0x2d.  Job guard: notify count at +0x00, reload value at +0x04, job
 * chain EA at +0x08 (64-bit), request SPU count at +0x10, auto reset at
 * +0x20, run-on-release at +0x30.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define POLICY_INVAL     0x80410802u
#define POLICY_SRCH      0x80410805u
#define POLICY_STAT      0x8041080Fu
#define CORE_STAT        0x8041070Fu
#define TASK_INVAL       0x80410902u
#define TASK_STAT        0x8041090Fu
#define TASK_ALIGN       0x80410910u
#define TASK_NULL        0x80410911u
#define JOB_INVAL        0x80410A02u
#define JOB_PERM         0x80410A09u
#define JOB_STAT         0x80410A0Fu
#define JOB_ALIGN        0x80410A10u
#define JOB_NULL         0x80410A11u

enum { WKL_PREPARING = 1, WKL_RUNNABLE = 2, WKL_SHUTTING_DOWN = 3, WKL_REMOVABLE = 4 };
#define MAX_WORKLOADS   32
#define JOB_REVISION_1  1

extern uint64_t cellSpursGetSpursAddress(void);
extern int _cellSpursSendWorkloadSignal(unsigned int wid);

static uint8_t line[128] __attribute__((aligned(128)));
static uint8_t small[16] __attribute__((aligned(16)));

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

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* 16 bytes of an object at ea (16-aligned) */
static const uint8_t *peek16(uint64_t ea)
{
	mfc_get(small, ea, 16, 31, 0, 0);
	mfc_write_tag_mask(1u << 31);
	(void)mfc_read_tag_status_all();
	return small;
}

/* offset of workload wid's byte in a 16-entry table within the 0x80 line */
static unsigned wkl_byte(unsigned wid, unsigned low, unsigned high)
{
	return (wid & 0x10) ? high + (wid & 0xf) : low + wid;
}

/* ---- workload shutdown --------------------------------------------------- */

int _cellSpursShutdownWorkload(unsigned int wid)
{
	uint64_t spurs = cellSpursGetSpursAddress();
	int shuttingDown;
	if (wid >= MAX_WORKLOADS)
		return (int)POLICY_INVAL;
	do {
		uint8_t *state, *event;
		line_get(spurs + 0x80);
		state = &line[wkl_byte(wid, 0x00, 0x50)];
		event = &line[wkl_byte(wid, 0x20, 0x70)];
		if (*state <= WKL_PREPARING)
			return (int)POLICY_STAT;
		if (*state == WKL_SHUTTING_DOWN || *state == WKL_REMOVABLE)
			return 0;
		/* SPUs still running it: the system service finishes the shutdown
		   once they leave; otherwise it can be removed right away */
		shuttingDown = line[wkl_byte(wid, 0x10, 0x60)] != 0;
		*state = shuttingDown ? WKL_SHUTTING_DOWN : WKL_REMOVABLE;
		if (shuttingDown)
			line[0x3d] = 0xff;          /* sysSrvMsgUpdateWorkload */
		else
			*event |= 1;
	} while (!line_put(spurs + 0x80));

	if (shuttingDown) {
		do {
			line_get(spurs);
			line[0x72] = 0xff;          /* sysSrvMessage */
		} while (!line_put(spurs));
	}
	return 0;
}

/* workload id of a taskset or job chain (+0x74) */
static unsigned object_wid(uint64_t ea)
{
	return be32(peek16(ea + 0x70) + 4);
}

int cellSpursShutdownTaskset(uint64_t eaTaskset)
{
	unsigned wid;
	int rc;
	if (!eaTaskset)
		return (int)TASK_NULL;
	if (eaTaskset & 0x7f)
		return (int)TASK_ALIGN;
	wid = object_wid(eaTaskset);
	if (wid >= MAX_WORKLOADS)
		return (int)TASK_INVAL;
	rc = _cellSpursShutdownWorkload(wid);
	return (unsigned)rc == POLICY_STAT ? (int)TASK_STAT : rc;
}

/* ---- job chains ---------------------------------------------------------- */

static int check_job_chain(uint64_t ea, unsigned *wid, const uint8_t **head)
{
	if (!ea)
		return (int)JOB_NULL;
	if (ea & 0x7f)
		return (int)JOB_ALIGN;
	*wid = object_wid(ea);
	if (*wid >= MAX_WORKLOADS)
		return (int)JOB_INVAL;
	if (head)
		*head = peek16(ea + 0x20);   /* autoReadyCount +0x24, jmVer +0x2d */
	return 0;
}

static int job_status(int rc)
{
	return (unsigned)rc == POLICY_STAT ? (int)JOB_STAT : rc;
}

int cellSpursShutdownJobChain(uint64_t eaJobChain)
{
	unsigned wid;
	int rc = check_job_chain(eaJobChain, &wid, 0);
	return rc ? rc : job_status(_cellSpursShutdownWorkload(wid));
}

int cellSpursRunJobChain(uint64_t eaJobChain)
{
	const uint8_t *head;
	unsigned wid;
	int rc = check_job_chain(eaJobChain, &wid, &head);
	if (rc)
		return rc;
	if (head[0x0d] <= JOB_REVISION_1)
		return (int)JOB_PERM;           /* old job chains are kicked */
	return job_status(_cellSpursSendWorkloadSignal(wid));
}

/* Set workload wid's ready count: to `value`, or with `onlyIfZero` from 0
 * to 1.  The workload must be enabled and runnable. */
static int ready_count(uint64_t spurs, unsigned wid, unsigned value, int onlyIfZero)
{
	uint8_t *count;
	do {
		line_get(spurs + 0x80);
		if (!(be32(&line[0x30]) & (0x80000000u >> wid)))
			return (int)POLICY_SRCH;
		if (line[wkl_byte(wid, 0x00, 0x50)] != WKL_RUNNABLE)
			return (int)POLICY_STAT;
		line_get(spurs);
		count = &line[wkl_byte(wid, 0x00, 0x10)];
		if (onlyIfZero) {
			if (*count)
				return 0;
			*count = 1;
		} else {
			*count = (uint8_t)value;
		}
	} while (!line_put(spurs));
	return 0;
}

int cellSpursKickJobChain(uint64_t eaJobChain, uint8_t numReadyCount)
{
	const uint8_t *head;
	unsigned wid, autoReady;
	int rc = check_job_chain(eaJobChain, &wid, &head);
	if (rc)
		return rc;
	if (head[0x0d] > JOB_REVISION_1)
		return (int)JOB_PERM;           /* newer job chains are run */
	autoReady = head[0x04];
	rc = ready_count(cellSpursGetSpursAddress(), wid, numReadyCount, !autoReady);
	return job_status(rc);
}

/* ---- job guard ----------------------------------------------------------- */

int cellSpursJobGuardInitialize(uint64_t eaJobChain, uint64_t eaJobGuard,
                                uint32_t notifyCount, uint8_t requestSpuCount,
                                uint8_t autoReset)
{
	unsigned wid, i;
	if (!eaJobChain || !eaJobGuard)
		return (int)JOB_NULL;
	if ((eaJobChain & 0x7f) || (eaJobGuard & 0x7f))
		return (int)JOB_ALIGN;
	wid = object_wid(eaJobChain);
	if (wid >= MAX_WORKLOADS)
		return (int)JOB_INVAL;
	for (i = 0; i < sizeof line; ++i)
		line[i] = 0;
	put_be32(&line[0x00], notifyCount);
	put_be32(&line[0x04], notifyCount);
	put_be32(&line[0x08], (uint32_t)(eaJobChain >> 32));
	put_be32(&line[0x0c], (uint32_t)eaJobChain);
	put_be32(&line[0x10], requestSpuCount);
	put_be32(&line[0x20], autoReset);
	mfc_putlluc(line, eaJobGuard, 0, 0);
	(void)mfc_read_atomic_status();
	return 0;
}

int cellSpursJobGuardReset(uint64_t eaJobGuard)
{
	if (!eaJobGuard)
		return (int)JOB_NULL;
	if (eaJobGuard & 0x7f)
		return (int)JOB_ALIGN;
	do {
		line_get(eaJobGuard);
		put_be32(&line[0x00], be32(&line[0x04]));
	} while (!line_put(eaJobGuard));
	return 0;
}

int cellSpursJobGuardNotify(uint64_t eaJobGuard)
{
	uint32_t left, runOnRelease, request;
	uint64_t chain;
	const uint8_t *head;
	if (!eaJobGuard)
		return (int)JOB_NULL;
	if (eaJobGuard & 0x7f)
		return (int)JOB_ALIGN;
	do {
		line_get(eaJobGuard);
		left = be32(&line[0x00]);
		if (!left)
			return (int)CORE_STAT;
		put_be32(&line[0x00], left - 1);
		runOnRelease = be32(&line[0x30]);
		request = be32(&line[0x10]);
		chain = (uint64_t)be32(&line[0x08]) << 32 | be32(&line[0x0c]);
	} while (!line_put(eaJobGuard));

	if (left > 1)
		return 0;
	/* the last notification releases the guard */
	head = peek16(chain + 0x20);
	if (head[0x0d] <= JOB_REVISION_1)
		return cellSpursKickJobChain(chain, (uint8_t)request);
	return runOnRelease ? cellSpursRunJobChain(chain) : 0;
}
