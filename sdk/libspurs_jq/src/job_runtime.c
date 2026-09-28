/* SPU job runtime initializer for jobs linked with -mspurs-job-initialize
 * (C and C++ jobs with static data, globals and virtual functions).
 *
 * __job_start (job_crt.S) runs, for every job:
 *   _cellSpursJobCrtAuxInitialize  clear .bss, relocate the image
 *   _init                          global constructors
 *   cellSpursJobMain2              the job
 *   __do_atexit                    atexit / static-object destructors
 *                                  (this runtime's own list; see below)
 *   _fini                          global destructors
 *   _cellSpursJobCrtAuxFinalize
 *
 * Relocation: a job is position-independent code, but data that holds
 * addresses (vtables, function and string pointer tables) is written at
 * link time for load address 0.  Linking with --emit-fixups makes ld
 * list those words in .fixup (one u32 per quadword: the quadword's
 * address | bit 8 >> n for each word n to relocate, ending with 0);
 * spurs_job.ld brackets it with __fixup_start.  The words are moved by
 * the difference between where the image runs and where it was last
 * relocated (except the job CRT header's _end / __bss_start words, which
 * the job-queue kernel reads as image offsets), which a self-pointing word that is itself in the table
 * records - so a cached image that runs again is not relocated twice.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>

typedef void (*init_fn)(void);

extern const uint32_t __fixup_start[] __attribute__((weak));
extern const char __job_crt_header_end[] __attribute__((weak));
extern char __bss_start[], _end[];
extern init_fn __ctors_start[] __attribute__((weak));
extern init_fn __ctors_end[] __attribute__((weak));
extern init_fn __dtors_start[] __attribute__((weak));
extern init_fn __dtors_end[] __attribute__((weak));
extern init_fn __init_array_start[] __attribute__((weak));
extern init_fn __init_array_end[] __attribute__((weak));
extern init_fn __fini_array_start[] __attribute__((weak));
extern init_fn __fini_array_end[] __attribute__((weak));


/* C++ static destructors register against this module handle */
void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;

/* the address the image's absolute words currently point at (see above) */
static uintptr_t s_relocatedFor __attribute__((used)) = (uintptr_t)&s_relocatedFor;

static void relocate(void)
{
	const uint32_t *r;
	const uintptr_t delta = (uintptr_t)&s_relocatedFor - *(volatile uintptr_t *)&s_relocatedFor;
	if (!__fixup_start || !delta)
		return;
	for (r = __fixup_start; *r; ++r) {
		uint32_t *q = (uint32_t *)((*r & ~15u) + delta);
		unsigned n;
		if ((const char *)q < __job_crt_header_end)
			continue;           /* the CRT header's words stay image offsets */
		for (n = 0; n < 4; ++n)
			if (*r & (8u >> n))
				q[n] += delta;
	}
}

int _cellSpursJobCrtAuxInitialize(void *ctx, void *job)
{
	uint32_t *p;
	(void)ctx;
	(void)job;
	for (p = (uint32_t *)__bss_start; p < (uint32_t *)_end; ++p)
		*p = 0;
	relocate();
	return 0;
}

void _cellSpursJobCrtAuxFinalize(void *ctx)
{
	(void)ctx;
}

void _init(void)
{
	init_fn *f;
	if (__ctors_start)
		for (f = __ctors_end; f > __ctors_start;) {
			--f;
			if (*f && *f != (init_fn)-1)
				(*f)();
		}
	if (__init_array_start)
		for (f = __init_array_start; f < __init_array_end; ++f)
			(*f)();
}

void _fini(void)
{
	init_fn *f;
	if (__fini_array_start)
		for (f = __fini_array_end; f > __fini_array_start;)
			(*--f)();
	if (__dtors_start)
		for (f = __dtors_start; f < __dtors_end; ++f)
			if (*f && *f != (init_fn)-1)
				(*f)();
}

/* atexit and __cxa_atexit (static objects with destructors).  Kept here
   rather than taken from newlib: the SPU libc is not position-independent
   and addresses its exit list at its link-time location, which in a job
   image is some other code's local store. */
#define JOB_ATEXIT_MAX 32

static struct {
	void (*fn)(void *);
	void *arg;
} s_exit[JOB_ATEXIT_MAX];
static unsigned s_exitCount;             /* .bss: empty at the start of every job */

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
	(void)dso;
	if (s_exitCount == JOB_ATEXIT_MAX)
		return -1;
	s_exit[s_exitCount].fn = fn;
	s_exit[s_exitCount].arg = arg;
	++s_exitCount;
	return 0;
}

static void call_plain(void *fn)
{
	((void (*)(void))fn)();
}

int atexit(void (*fn)(void))
{
	return __cxa_atexit(call_plain, (void *)fn, 0);
}

/* run them newest first */
void __do_atexit(void)
{
	while (s_exitCount) {
		--s_exitCount;
		s_exit[s_exitCount].fn(s_exit[s_exitCount].arg);
	}
}

/* Function-local statics: a job runs on one SPU and nothing else touches
   its local store, so the guards need no locking.  Defining them here
   also keeps libsupc++'s versions, and the exception, terminate and
   demangler code they bring, out of job images. */
int __cxa_guard_acquire(uint64_t *guard)
{
	return !*(volatile uint8_t *)guard;
}

void __cxa_guard_release(uint64_t *guard)
{
	*(volatile uint8_t *)guard = 1;
}

void __cxa_guard_abort(uint64_t *guard)
{
	(void)guard;
}
