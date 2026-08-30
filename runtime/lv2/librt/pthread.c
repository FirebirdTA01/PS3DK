/*
 * pthread.c — POSIX threads over the Lv-2 primitives.
 *
 * Third-party libraries detect threading through autoconf/CMake probes that
 * compile and link a small POSIX program.  Without an implementation those
 * probes fail and the library silently falls back to a no-threads tier —
 * cairo to -DCAIRO_NO_MUTEX, pixman to -DPIXMAN_NO_TLS — which turns their
 * internal caches into unguarded shared globals.  This file provides the
 * POSIX surface those probes look for, so the libraries build unpatched.
 *
 * Mapping:
 *   pthread_mutex_*   -> sysMutex*  (Lv-2 syscalls 100-104)
 *   pthread_cond_*    -> sysCond*   (Lv-2 syscalls 105-109)
 *   pthread_create/…  -> sysThread* (Lv-2 syscalls 43-49 + sysPrxForUser)
 *
 * Three properties of the Lv-2 API shape the design:
 *
 *   * A mutex id is not a compile-time constant — it comes back from
 *     syscall 100.  PTHREAD_MUTEX_INITIALIZER therefore stores a sentinel
 *     (see <pthread.h>) and the real mutex is created on first use, under
 *     one process-wide lock that is itself bootstrapped with a compare-and-
 *     swap.  Same for PTHREAD_COND_INITIALIZER.
 *
 *   * sysCondCreate binds a condition variable to ONE mutex at creation
 *     time, while POSIX binds at first wait.  So cond creation is deferred
 *     all the way to pthread_cond_wait, which is the first point where the
 *     mutex is known.  Signalling a never-waited-on cond is a no-op, which
 *     is exactly the POSIX semantics (no waiters).
 *
 *   * Lv-2 has no thread-specific-data.  Keys live in a fixed table indexed
 *     by a slot that is found from sys_ppu_thread_get_id().  The table is
 *     bounded (PT_THREADS_MAX x PT_KEYS_MAX); pthread_key_create and
 *     pthread_setspecific report EAGAIN when it is exhausted rather than
 *     corrupting anything.
 *
 * Deliberately NOT implemented, because Lv-2 offers nothing to build them
 * on: cancellation (pthread_cancel returns ENOSYS; setcancelstate /
 * setcanceltype accept and remember nothing), and stack unwinding through
 * pthread_exit (cleanup handlers pushed with pthread_cleanup_push run only
 * when popped with a non-zero argument).
 *
 * Errno note: this file includes <errno.h> and NOT <sys/synchronization.h>,
 * which redefines EBUSY to the Lv-2 value 0x8001000A.  Everything here
 * returns POSIX errno values — pthread_mutex_trylock returns EBUSY == 16 —
 * and lv2error() from <sys/lv2errno.h> does the translation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <sched.h>

#include <ppu-types.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sys/lv2errno.h>
#include <lv2/thread.h>

/* ------------------------------------------------------------------ *
 * Tunables
 * ------------------------------------------------------------------ */

/* Threads that can hold thread-specific data at one time, and keys that can
 * exist at one time.  The table is static: PT_THREADS_MAX * PT_KEYS_MAX
 * pointers of BSS, pulled into the link only when a program actually uses
 * pthreads.  cairo needs no keys at all and pixman needs exactly one. */
#define PT_THREADS_MAX   32
#define PT_KEYS_MAX      32

/* POSIX requires at least PTHREAD_DESTRUCTOR_ITERATIONS (4) passes over the
 * key table at thread exit, because a destructor may set a value again. */
#define PT_DTOR_ITERATIONS 4

/* Used when pthread_create gets no attributes, or attributes that leave the
 * defaults in place.  0x1000 (what the PSL1GHT samples pass) is too small
 * for POSIX-shaped code that expects a real stack. */
#define PT_DEFAULT_STACK_SIZE  0x10000u
#define PT_DEFAULT_PRIORITY    1000

/* Sentinels stored in a pthread_mutex_t / pthread_cond_t that has not been
 * created yet.  Kept in sync with <pthread.h>; Lv-2 never returns an id in
 * the 0xFFFFFFF0..0xFFFFFFFF range, and 0 is treated as "not created" too so
 * that zero-filled (calloc'd, memset) objects behave. */
#define PT_M_UNINIT       0x00000000u
#define PT_M_ERRORCHECK   0xFFFFFFFDu
#define PT_M_RECURSIVE    0xFFFFFFFEu
#define PT_M_NORMAL       0xFFFFFFFFu
#define PT_SENTINEL_FIRST 0xFFFFFFF0u

#define PT_C_UNINIT       0x00000000u
#define PT_C_STATIC       0xFFFFFFFFu

/* CELL_EDEADLK.  Needed by name in pthread_mutex_trylock, where the POSIX
 * answer differs from the straight lv2error() translation. */
#define PT_LV2_EDEADLK    ((s32)0x80010008)

/* ------------------------------------------------------------------ *
 * Error translation
 * ------------------------------------------------------------------ */

static int
pt_err(s32 rc)
{
	if (rc == 0)
		return 0;
	return (int)lv2error(rc);
}

/* ------------------------------------------------------------------ *
 * Process-wide locks
 *
 * pt_table_mutex serialises every table mutation and every lazy creation.
 * It is only ever held across syscalls, never across user code, so it can
 * not be held for an unbounded time.
 *
 * pt_once_mutex is separate precisely because pthread_once DOES run user
 * code under it: a once-routine that creates a mutex, or that starts a
 * thread which touches thread-specific data, must not be blocked by (or
 * block) the table lock.  Both are recursive so that a once-routine calling
 * pthread_once, or a destructor calling back in, does not self-deadlock.
 * ------------------------------------------------------------------ */

#define PT_BOOT_UNSTARTED  0u
#define PT_BOOT_RUNNING    1u
#define PT_BOOT_READY      2u
#define PT_BOOT_FAILED     3u

static unsigned int pt_boot_state;
static sys_mutex_t  pt_table_mutex;
static sys_mutex_t  pt_once_mutex;

static void
pt_mutex_attr_init(sys_mutex_attr_t *attr, int recursive)
{
	memset(attr, 0, sizeof(*attr));
	attr->attr_protocol  = SYS_MUTEX_PROTOCOL_PRIO;
	attr->attr_recursive = recursive ? SYS_MUTEX_ATTR_RECURSIVE
	                                 : SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr->attr_pshared   = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr->attr_adaptive  = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
}

/* Create the two process-wide locks exactly once.  The winner of the
 * compare-and-swap creates them; everyone else spins on a yield until the
 * state settles.  This runs before any other thread can exist in the common
 * case (the main thread reaches it first), so the spin is a formality. */
static int
pt_bootstrap(void)
{
	unsigned int expected = PT_BOOT_UNSTARTED;
	unsigned int state;

	state = __atomic_load_n(&pt_boot_state, __ATOMIC_ACQUIRE);
	if (state == PT_BOOT_READY)
		return 0;
	if (state == PT_BOOT_FAILED)
		return ENOMEM;

	if (__atomic_compare_exchange_n(&pt_boot_state, &expected,
	                                PT_BOOT_RUNNING, 0,
	                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		sys_mutex_attr_t attr;
		s32 rc;

		pt_mutex_attr_init(&attr, 1);
		rc = sysMutexCreate(&pt_table_mutex, &attr);
		if (rc == 0) {
			pt_mutex_attr_init(&attr, 1);
			rc = sysMutexCreate(&pt_once_mutex, &attr);
			if (rc != 0)
				(void)sysMutexDestroy(pt_table_mutex);
		}
		if (rc != 0) {
			__atomic_store_n(&pt_boot_state, PT_BOOT_FAILED,
			                 __ATOMIC_RELEASE);
			return pt_err(rc);
		}
		__atomic_store_n(&pt_boot_state, PT_BOOT_READY, __ATOMIC_RELEASE);
		return 0;
	}

	for (;;) {
		state = __atomic_load_n(&pt_boot_state, __ATOMIC_ACQUIRE);
		if (state == PT_BOOT_READY)
			return 0;
		if (state == PT_BOOT_FAILED)
			return ENOMEM;
		(void)sysThreadYield();
	}
}

static int
pt_table_lock(void)
{
	int err = pt_bootstrap();

	if (err != 0)
		return err;
	return pt_err(sysMutexLock(pt_table_mutex, 0));
}

static void
pt_table_unlock(void)
{
	(void)sysMutexUnlock(pt_table_mutex);
}

/* ------------------------------------------------------------------ *
 * Thread table
 * ------------------------------------------------------------------ */

struct pt_thread {
	unsigned char    used;
	unsigned char    finished;   /* start routine returned / pthread_exit */
	unsigned char    detached;
	unsigned char    foreign;    /* not created through pthread_create */
	sys_ppu_thread_t lv2_id;
	void            *retval;
	void          *(*start)(void *);
	void            *arg;
	void            *tsd[PT_KEYS_MAX];
};

struct pt_key {
	unsigned char used;
	void        (*destructor)(void *);
};

static struct pt_thread pt_threads[PT_THREADS_MAX];
static struct pt_key    pt_keys[PT_KEYS_MAX];

/* A pthread_t is a 1-based index into pt_threads, never a raw Lv-2 id:
 * sys_ppu_thread_t is 64 bits and pthread_t is 32, so the raw id does not
 * fit.  0 is therefore always an invalid pthread_t. */
static struct pt_thread *
pt_slot_from_handle(pthread_t handle)
{
	unsigned int idx = (unsigned int)handle;

	if (idx == 0 || idx > PT_THREADS_MAX)
		return NULL;
	if (!pt_threads[idx - 1].used)
		return NULL;
	return &pt_threads[idx - 1];
}

static pthread_t
pt_handle_from_slot(const struct pt_thread *slot)
{
	return (pthread_t)((slot - pt_threads) + 1);
}

/* Caller holds the table lock.  Registers the calling thread on first use;
 * threads that were not started through pthread_create (the main thread, or
 * one from a raw sys_ppu_thread_create) get a detached slot so that nothing
 * ever waits to join them. */
static int
pt_self_slot_locked(struct pt_thread **out)
{
	sys_ppu_thread_t id = 0;
	struct pt_thread *free_slot = NULL;
	int i;

	if (sysThreadGetId(&id) != 0)
		return ESRCH;

	for (i = 0; i < PT_THREADS_MAX; i++) {
		struct pt_thread *t = &pt_threads[i];

		if (t->used) {
			if (t->lv2_id == id && !t->finished) {
				*out = t;
				return 0;
			}
		} else if (free_slot == NULL) {
			free_slot = t;
		}
	}

	if (free_slot == NULL)
		return EAGAIN;

	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used     = 1;
	free_slot->foreign  = 1;
	free_slot->detached = 1;
	free_slot->lv2_id   = id;
	*out = free_slot;
	return 0;
}

/* Run the thread-specific-data destructors for the calling thread.  The
 * table lock is dropped around each destructor: POSIX lets a destructor
 * call back into the pthread API, and holding a process-wide lock across
 * arbitrary user code is how a shim like this deadlocks. */
static void
pt_run_tsd_destructors(struct pt_thread *self)
{
	int iteration;

	for (iteration = 0; iteration < PT_DTOR_ITERATIONS; iteration++) {
		int progress = 0;
		int i;

		for (i = 0; i < PT_KEYS_MAX; i++) {
			void (*destructor)(void *);
			void *value;

			if (pt_table_lock() != 0)
				return;
			value      = self->tsd[i];
			destructor = pt_keys[i].used ? pt_keys[i].destructor : NULL;
			if (value != NULL && destructor != NULL)
				self->tsd[i] = NULL;
			else
				destructor = NULL;
			pt_table_unlock();

			if (destructor != NULL) {
				destructor(value);
				progress = 1;
			}
		}

		if (!progress)
			return;
	}
}

/* Common tail for a thread that is going away.  Returns with the slot
 * either released (detached) or parked for pthread_join. */
static void
pt_thread_finish(struct pt_thread *self, void *retval)
{
	pt_run_tsd_destructors(self);

	if (pt_table_lock() != 0)
		return;
	self->retval   = retval;
	self->finished = 1;
	if (self->detached)
		self->used = 0;
	pt_table_unlock();
}

static void
pt_trampoline(void *arg)
{
	struct pt_thread *self = (struct pt_thread *)arg;
	void *retval;

	retval = self->start(self->arg);
	pt_thread_finish(self, retval);
	sysThreadExit(0);
}

/* ------------------------------------------------------------------ *
 * Mutexes
 * ------------------------------------------------------------------ */

static int
pt_is_uncreated(unsigned int value)
{
	return value == PT_M_UNINIT || value >= PT_SENTINEL_FIRST;
}

/* Resolve a pthread_mutex_t to a live Lv-2 mutex id, creating it if this is
 * a statically initialised mutex being used for the first time. */
static int
pt_mutex_resolve(pthread_mutex_t *mutex, sys_mutex_t *out)
{
	unsigned int value;
	int err;

	if (mutex == NULL)
		return EINVAL;

	value = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
	if (!pt_is_uncreated(value)) {
		*out = (sys_mutex_t)value;
		return 0;
	}

	err = pt_table_lock();
	if (err != 0)
		return err;

	value = __atomic_load_n(mutex, __ATOMIC_RELAXED);
	if (pt_is_uncreated(value)) {
		sys_mutex_attr_t attr;
		sys_mutex_t id = 0;
		s32 rc;

		pt_mutex_attr_init(&attr, value == PT_M_RECURSIVE);
		rc = sysMutexCreate(&id, &attr);
		if (rc != 0) {
			pt_table_unlock();
			return pt_err(rc);
		}
		value = (unsigned int)id;
		__atomic_store_n(mutex, value, __ATOMIC_RELEASE);
	}

	pt_table_unlock();
	*out = (sys_mutex_t)value;
	return 0;
}

int
pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	sys_mutex_attr_t sattr;
	sys_mutex_t id = 0;
	int recursive = 0;
	s32 rc;

	if (mutex == NULL)
		return EINVAL;
	if (attr != NULL && attr->recursive == PTHREAD_MUTEX_RECURSIVE)
		recursive = 1;

	pt_mutex_attr_init(&sattr, recursive);
	rc = sysMutexCreate(&id, &sattr);
	if (rc != 0)
		return pt_err(rc);

	__atomic_store_n(mutex, (unsigned int)id, __ATOMIC_RELEASE);
	return 0;
}

int
pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	unsigned int value;
	s32 rc;

	if (mutex == NULL)
		return EINVAL;

	value = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
	if (pt_is_uncreated(value)) {
		/* Never used, so nothing was ever created. */
		__atomic_store_n(mutex, PT_M_UNINIT, __ATOMIC_RELEASE);
		return 0;
	}

	rc = sysMutexDestroy((sys_mutex_t)value);
	if (rc != 0)
		return pt_err(rc);

	__atomic_store_n(mutex, PT_M_UNINIT, __ATOMIC_RELEASE);
	return 0;
}

int
pthread_mutex_lock(pthread_mutex_t *mutex)
{
	sys_mutex_t id;
	int err = pt_mutex_resolve(mutex, &id);

	if (err != 0)
		return err;
	return pt_err(sysMutexLock(id, 0));
}

int
pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	sys_mutex_t id;
	s32 rc;
	int err = pt_mutex_resolve(mutex, &id);

	if (err != 0)
		return err;

	rc = sysMutexTryLock(id);
	if (rc == 0)
		return 0;

	/* Lv-2 splits the "already locked" case in two: a mutex held by
	 * ANOTHER thread gives CELL_EBUSY, but a non-recursive mutex held by
	 * the CALLING thread gives CELL_EDEADLK (sys_mutex.h try_lock:
	 * `if (it.owner == cpu.id) { ...recursive... return CELL_EDEADLK; }`).
	 * POSIX makes no such split for trylock — it fails with EBUSY because
	 * the mutex could not be acquired, whoever holds it — so fold the
	 * deadlock status in here.  pthread_mutex_lock deliberately does NOT
	 * do this: reporting EDEADLK to a caller that is about to self-
	 * deadlock is more useful than blocking forever. */
	if (rc == PT_LV2_EDEADLK)
		return EBUSY;

	/* Everything else translates normally; CELL_EBUSY (0x8001000A)
	 * becomes POSIX EBUSY (16) because this file does not include
	 * <sys/synchronization.h>, which redefines it. */
	return pt_err(rc);
}

int
pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	unsigned int value;

	if (mutex == NULL)
		return EINVAL;

	value = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
	if (pt_is_uncreated(value))
		return EPERM;   /* never locked, so not owned */

	return pt_err(sysMutexUnlock((sys_mutex_t)value));
}

/* Microseconds from now until an absolute CLOCK_REALTIME deadline.  Lv-2
 * reads a timeout of 0 as "wait forever", so a deadline that has already
 * passed is clamped to the shortest possible wait rather than to 0. */
static u64
pt_timeout_usec(const struct timespec *abstime)
{
	u64 now_sec = 0, now_nsec = 0;
	s64 delta_usec;

	if (sysGetCurrentTime(&now_sec, &now_nsec) != 0)
		return 1;

	delta_usec = ((s64)abstime->tv_sec - (s64)now_sec) * 1000000
	           + ((s64)abstime->tv_nsec - (s64)now_nsec) / 1000;

	if (delta_usec < 1)
		return 1;
	return (u64)delta_usec;
}

int
pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
	sys_mutex_t id;
	int err;

	if (abstime == NULL)
		return EINVAL;
	err = pt_mutex_resolve(mutex, &id);
	if (err != 0)
		return err;
	return pt_err(sysMutexLock(id, pt_timeout_usec(abstime)));
}

/* ------------------------------------------------------------------ *
 * Mutex attributes
 *
 * The mutex type is stored in the `recursive` member, which is present in
 * pthread_mutexattr_t whether or not _UNIX98_THREAD_MUTEX_ATTRIBUTES is on;
 * see the comment in <pthread.h> for why we must not switch that macro on.
 * ------------------------------------------------------------------ */

int
pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));
	attr->is_initialized = 1;
	attr->recursive      = PTHREAD_MUTEX_NORMAL;
	return 0;
}

int
pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	attr->is_initialized = 0;
	return 0;
}

int
pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind)
{
	if (attr == NULL)
		return EINVAL;
	if (kind != PTHREAD_MUTEX_NORMAL && kind != PTHREAD_MUTEX_RECURSIVE
	    && kind != PTHREAD_MUTEX_ERRORCHECK && kind != PTHREAD_MUTEX_DEFAULT)
		return EINVAL;
	/* ERRORCHECK and DEFAULT both land on a non-recursive Lv-2 mutex:
	 * the kernel primitive is recursive or it is not, with no
	 * relock-detection mode in between. */
	attr->recursive = kind;
	return 0;
}

int
pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind)
{
	if (attr == NULL || kind == NULL)
		return EINVAL;
	*kind = attr->recursive;
	return 0;
}

int
pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
	if (attr == NULL)
		return EINVAL;
	/* Lv-2 mutexes live inside one process; PTHREAD_PROCESS_SHARED (1)
	 * cannot be honoured. */
	if (pshared != 0)
		return ENOTSUP;
	return 0;
}

int
pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared)
{
	if (attr == NULL || pshared == NULL)
		return EINVAL;
	*pshared = 0;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Condition variables
 * ------------------------------------------------------------------ */

int
pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	(void)attr;
	if (cond == NULL)
		return EINVAL;
	/* Creation is deferred to the first wait: sysCondCreate needs the
	 * mutex, and POSIX does not name one until then. */
	__atomic_store_n(cond, PT_C_STATIC, __ATOMIC_RELEASE);
	return 0;
}

int
pthread_cond_destroy(pthread_cond_t *cond)
{
	unsigned int value;
	s32 rc;

	if (cond == NULL)
		return EINVAL;

	value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	if (value == PT_C_UNINIT || value == PT_C_STATIC) {
		__atomic_store_n(cond, PT_C_UNINIT, __ATOMIC_RELEASE);
		return 0;
	}

	rc = sysCondDestroy((sys_cond_t)value);
	if (rc != 0)
		return pt_err(rc);

	__atomic_store_n(cond, PT_C_UNINIT, __ATOMIC_RELEASE);
	return 0;
}

/* Resolve a pthread_cond_t against the mutex the caller is waiting with,
 * creating the Lv-2 condition variable on first wait.
 *
 * A cond used with two different mutexes is undefined behaviour in POSIX;
 * here the first mutex wins and later waits keep using it. */
static int
pt_cond_resolve(pthread_cond_t *cond, sys_mutex_t mutex_id, sys_cond_t *out)
{
	unsigned int value;
	int err;

	if (cond == NULL)
		return EINVAL;

	value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	if (value != PT_C_UNINIT && value != PT_C_STATIC) {
		*out = (sys_cond_t)value;
		return 0;
	}

	err = pt_table_lock();
	if (err != 0)
		return err;

	value = __atomic_load_n(cond, __ATOMIC_RELAXED);
	if (value == PT_C_UNINIT || value == PT_C_STATIC) {
		sys_cond_attr_t attr;
		sys_cond_t id = 0;
		s32 rc;

		memset(&attr, 0, sizeof(attr));
		/* Misnomer in <sys/cond.h>: SYS_COND_ATTR_PSHARED is 0x200,
		 * which <sys/synchronization.h> spells SYS_SYNC_NOT_PROCESS_-
		 * SHARED — the not-shared value, and what PSL1GHT's own
		 * sysCondAttrInitialize stores.  Process-shared conditions are
		 * not something this shim can offer anyway. */
		attr.attr_pshared = SYS_COND_ATTR_PSHARED;
		rc = sysCondCreate(&id, mutex_id, &attr);
		if (rc != 0) {
			pt_table_unlock();
			return pt_err(rc);
		}
		value = (unsigned int)id;
		__atomic_store_n(cond, value, __ATOMIC_RELEASE);
	}

	pt_table_unlock();
	*out = (sys_cond_t)value;
	return 0;
}

static int
pt_cond_wait_common(pthread_cond_t *cond, pthread_mutex_t *mutex, u64 timeout)
{
	sys_mutex_t mutex_id;
	sys_cond_t cond_id;
	int err;

	err = pt_mutex_resolve(mutex, &mutex_id);
	if (err != 0)
		return err;
	err = pt_cond_resolve(cond, mutex_id, &cond_id);
	if (err != 0)
		return err;
	return pt_err(sysCondWait(cond_id, timeout));
}

int
pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	return pt_cond_wait_common(cond, mutex, 0);
}

int
pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                       const struct timespec *abstime)
{
	if (abstime == NULL)
		return EINVAL;
	return pt_cond_wait_common(cond, mutex, pt_timeout_usec(abstime));
}

int
pthread_cond_signal(pthread_cond_t *cond)
{
	unsigned int value;

	if (cond == NULL)
		return EINVAL;

	value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	/* Not created yet means nobody has ever waited on it, and POSIX makes
	 * signalling a condition with no waiters a no-op. */
	if (value == PT_C_UNINIT || value == PT_C_STATIC)
		return 0;

	return pt_err(sysCondSignal((sys_cond_t)value));
}

int
pthread_cond_broadcast(pthread_cond_t *cond)
{
	unsigned int value;

	if (cond == NULL)
		return EINVAL;

	value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	if (value == PT_C_UNINIT || value == PT_C_STATIC)
		return 0;

	return pt_err(sysCondBroadcast((sys_cond_t)value));
}

int
pthread_condattr_init(pthread_condattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));
	attr->is_initialized = 1;
	attr->clock          = CLOCK_REALTIME;
	return 0;
}

int
pthread_condattr_destroy(pthread_condattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	attr->is_initialized = 0;
	return 0;
}

int
pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
	if (attr == NULL)
		return EINVAL;
	/* sysCondWait measures a relative timeout, which pt_timeout_usec
	 * derives from the wall clock; there is no second clock to select. */
	if (clock_id != CLOCK_REALTIME)
		return ENOTSUP;
	attr->clock = clock_id;
	return 0;
}

int
pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock_id)
{
	if (attr == NULL || clock_id == NULL)
		return EINVAL;
	*clock_id = CLOCK_REALTIME;
	return 0;
}

int
pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
	if (attr == NULL)
		return EINVAL;
	if (pshared != 0)
		return ENOTSUP;
	return 0;
}

int
pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
	if (attr == NULL || pshared == NULL)
		return EINVAL;
	*pshared = 0;
	return 0;
}

/* ------------------------------------------------------------------ *
 * One-time initialisation
 * ------------------------------------------------------------------ */

int
pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
	int err;

	if (once_control == NULL || init_routine == NULL)
		return EINVAL;

	if (__atomic_load_n(&once_control->init_executed, __ATOMIC_ACQUIRE))
		return 0;

	err = pt_bootstrap();
	if (err != 0)
		return err;
	err = pt_err(sysMutexLock(pt_once_mutex, 0));
	if (err != 0)
		return err;

	if (!once_control->init_executed) {
		init_routine();
		__atomic_store_n(&once_control->init_executed, 1, __ATOMIC_RELEASE);
	}

	(void)sysMutexUnlock(pt_once_mutex);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Thread-specific data
 * ------------------------------------------------------------------ */

int
pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	int err;
	int i;

	if (key == NULL)
		return EINVAL;

	err = pt_table_lock();
	if (err != 0)
		return err;

	for (i = 0; i < PT_KEYS_MAX; i++) {
		if (!pt_keys[i].used) {
			int t;

			pt_keys[i].used       = 1;
			pt_keys[i].destructor = destructor;
			/* A key index can be reused after pthread_key_delete;
			 * POSIX requires the new key to start out NULL in every
			 * thread. */
			for (t = 0; t < PT_THREADS_MAX; t++)
				pt_threads[t].tsd[i] = NULL;
			pt_table_unlock();
			*key = (pthread_key_t)(i + 1);
			return 0;
		}
	}

	pt_table_unlock();
	return EAGAIN;
}

int
pthread_key_delete(pthread_key_t key)
{
	unsigned int idx = (unsigned int)key;
	int err;

	if (idx == 0 || idx > PT_KEYS_MAX)
		return EINVAL;

	err = pt_table_lock();
	if (err != 0)
		return err;

	if (!pt_keys[idx - 1].used) {
		pt_table_unlock();
		return EINVAL;
	}
	pt_keys[idx - 1].used       = 0;
	pt_keys[idx - 1].destructor = NULL;
	pt_table_unlock();
	return 0;
}

int
pthread_setspecific(pthread_key_t key, const void *value)
{
	unsigned int idx = (unsigned int)key;
	struct pt_thread *self;
	int err;

	if (idx == 0 || idx > PT_KEYS_MAX)
		return EINVAL;

	err = pt_table_lock();
	if (err != 0)
		return err;

	if (!pt_keys[idx - 1].used) {
		pt_table_unlock();
		return EINVAL;
	}

	err = pt_self_slot_locked(&self);
	if (err != 0) {
		pt_table_unlock();
		return err;
	}

	self->tsd[idx - 1] = (void *)value;
	pt_table_unlock();
	return 0;
}

void *
pthread_getspecific(pthread_key_t key)
{
	unsigned int idx = (unsigned int)key;
	struct pt_thread *self;
	void *value = NULL;

	if (idx == 0 || idx > PT_KEYS_MAX)
		return NULL;
	if (pt_table_lock() != 0)
		return NULL;

	if (pt_keys[idx - 1].used && pt_self_slot_locked(&self) == 0)
		value = self->tsd[idx - 1];

	pt_table_unlock();
	return value;
}

/* ------------------------------------------------------------------ *
 * Thread attributes
 * ------------------------------------------------------------------ */

int
pthread_attr_init(pthread_attr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));
	attr->is_initialized = 1;
	attr->stackaddr      = NULL;
	attr->stacksize      = (int)PT_DEFAULT_STACK_SIZE;
	attr->contentionscope = PTHREAD_SCOPE_PROCESS;
	attr->inheritsched   = PTHREAD_INHERIT_SCHED;
	attr->schedpolicy    = SCHED_OTHER;
	attr->schedparam.sched_priority = PT_DEFAULT_PRIORITY;
	attr->detachstate    = PTHREAD_CREATE_JOINABLE;
	return 0;
}

int
pthread_attr_destroy(pthread_attr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	attr->is_initialized = 0;
	return 0;
}

int
pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
	if (attr == NULL)
		return EINVAL;
	if (detachstate != PTHREAD_CREATE_JOINABLE
	    && detachstate != PTHREAD_CREATE_DETACHED)
		return EINVAL;
	attr->detachstate = detachstate;
	return 0;
}

int
pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
	if (attr == NULL || detachstate == NULL)
		return EINVAL;
	*detachstate = attr->detachstate;
	return 0;
}

int
pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
	if (attr == NULL)
		return EINVAL;
	if (stacksize == 0 || stacksize > 0x7FFFFFFFu)
		return EINVAL;
	attr->stacksize = (int)stacksize;
	return 0;
}

int
pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
	if (attr == NULL || stacksize == NULL)
		return EINVAL;
	*stacksize = (size_t)attr->stacksize;
	return 0;
}

int
pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr)
{
	if (attr == NULL)
		return EINVAL;
	/* sys_ppu_thread_create always allocates its own stack; remember the
	 * request so getstackaddr round-trips, but it is not honoured. */
	attr->stackaddr = stackaddr;
	return 0;
}

int
pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr)
{
	if (attr == NULL || stackaddr == NULL)
		return EINVAL;
	*stackaddr = attr->stackaddr;
	return 0;
}

int
pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize)
{
	int err = pthread_attr_setstacksize(attr, stacksize);

	if (err != 0)
		return err;
	return pthread_attr_setstackaddr(attr, stackaddr);
}

int
pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
                      size_t *stacksize)
{
	int err = pthread_attr_getstackaddr(attr, stackaddr);

	if (err != 0)
		return err;
	return pthread_attr_getstacksize(attr, stacksize);
}

int
pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
	if (attr == NULL)
		return EINVAL;
	(void)guardsize;   /* Lv-2 stacks carry no user-selectable guard page */
	return 0;
}

int
pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize)
{
	if (attr == NULL || guardsize == NULL)
		return EINVAL;
	*guardsize = 0;
	return 0;
}

int
pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
	if (attr == NULL || param == NULL)
		return EINVAL;
	attr->schedparam = *param;
	return 0;
}

int
pthread_attr_getschedparam(const pthread_attr_t *attr, struct sched_param *param)
{
	if (attr == NULL || param == NULL)
		return EINVAL;
	*param = attr->schedparam;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Threads
 * ------------------------------------------------------------------ */

int
pthread_create(pthread_t *thread, const pthread_attr_t *attr,
               void *(*start_routine)(void *), void *arg)
{
	struct pt_thread *slot = NULL;
	sys_ppu_thread_t id = 0;
	u64 stacksize = PT_DEFAULT_STACK_SIZE;
	s32 priority = PT_DEFAULT_PRIORITY;
	int detached = 0;
	int err;
	int i;
	s32 rc;

	if (thread == NULL || start_routine == NULL)
		return EINVAL;

	if (attr != NULL && attr->is_initialized) {
		if (attr->stacksize > 0)
			stacksize = (u64)attr->stacksize;
		if (attr->schedparam.sched_priority > 0)
			priority = (s32)attr->schedparam.sched_priority;
		detached = (attr->detachstate == PTHREAD_CREATE_DETACHED);
	}

	err = pt_table_lock();
	if (err != 0)
		return err;

	for (i = 0; i < PT_THREADS_MAX; i++) {
		if (!pt_threads[i].used) {
			slot = &pt_threads[i];
			break;
		}
	}
	if (slot == NULL) {
		pt_table_unlock();
		return EAGAIN;
	}

	memset(slot, 0, sizeof(*slot));
	slot->used     = 1;
	slot->detached = (unsigned char)detached;
	slot->start    = start_routine;
	slot->arg      = arg;

	/* A detached thread is created NOT joinable, so that Lv-2 reaps it on
	 * exit.  Creating it joinable and never joining would strand the
	 * kernel thread object for the life of the process.
	 *
	 * The table lock is deliberately held across the create: the new
	 * thread may reach pthread_self / pthread_getspecific before we get
	 * to store its Lv-2 id, and without the id in the slot it would
	 * register itself a second time as a foreign thread. */
	rc = sysThreadCreate(&id, pt_trampoline, slot, priority, stacksize,
	                     detached ? 0 : THREAD_JOINABLE, (char *)"pthread");
	if (rc != 0) {
		slot->used = 0;
		pt_table_unlock();
		return pt_err(rc);
	}

	slot->lv2_id = id;
	*thread = pt_handle_from_slot(slot);
	pt_table_unlock();
	return 0;
}

int
pthread_join(pthread_t thread, void **value_ptr)
{
	struct pt_thread *slot;
	sys_ppu_thread_t id;
	u64 lv2_retval = 0;
	int err;
	s32 rc;

	err = pt_table_lock();
	if (err != 0)
		return err;

	slot = pt_slot_from_handle(thread);
	if (slot == NULL || slot->foreign) {
		pt_table_unlock();
		return ESRCH;
	}
	if (slot->detached) {
		pt_table_unlock();
		return EINVAL;
	}
	id = slot->lv2_id;
	pt_table_unlock();

	rc = sysThreadJoin(id, &lv2_retval);
	if (rc != 0)
		return pt_err(rc);

	err = pt_table_lock();
	if (err != 0)
		return err;
	if (value_ptr != NULL)
		*value_ptr = slot->retval;
	slot->used = 0;
	pt_table_unlock();
	return 0;
}

int
pthread_detach(pthread_t thread)
{
	struct pt_thread *slot;
	int err = pt_table_lock();

	if (err != 0)
		return err;

	slot = pt_slot_from_handle(thread);
	if (slot == NULL) {
		pt_table_unlock();
		return ESRCH;
	}
	if (slot->detached) {
		pt_table_unlock();
		return EINVAL;
	}

	/* Every slot that reaches here belongs to a thread created JOINABLE
	 * (pthread_create makes an already-detached thread non-joinable, and
	 * such a slot is rejected above), so the Lv-2 thread object needs
	 * releasing whether or not the thread has run to completion —
	 * detaching only the still-running ones would strand the rest. */
	slot->detached = 1;
	(void)sysThreadDetach(slot->lv2_id);
	if (slot->finished)
		slot->used = 0;
	pt_table_unlock();
	return 0;
}

void
pthread_exit(void *value_ptr)
{
	struct pt_thread *self = NULL;

	if (pt_table_lock() == 0) {
		if (pt_self_slot_locked(&self) != 0)
			self = NULL;
		pt_table_unlock();
	}

	if (self != NULL)
		pt_thread_finish(self, value_ptr);

	sysThreadExit(0);
	__builtin_unreachable();
}

pthread_t
pthread_self(void)
{
	struct pt_thread *self;
	pthread_t handle = 0;

	if (pt_table_lock() != 0)
		return 0;
	if (pt_self_slot_locked(&self) == 0)
		handle = pt_handle_from_slot(self);
	pt_table_unlock();
	return handle;
}

int
pthread_equal(pthread_t t1, pthread_t t2)
{
	return t1 == t2;
}

void
pthread_yield(void)
{
	(void)sysThreadYield();
}

int
sched_yield(void)
{
	return pt_err(sysThreadYield());
}

/* ------------------------------------------------------------------ *
 * Cancellation and fork handlers
 *
 * Lv-2 has no way to interrupt a thread at a cancellation point, so
 * cancellation is refused rather than faked.  The state setters still
 * succeed, because library code routinely brackets critical sections with
 * them and only checks the return in debug builds; reporting failure there
 * would break callers that are not actually relying on cancellation.
 * ------------------------------------------------------------------ */

int
pthread_cancel(pthread_t thread)
{
	(void)thread;
	return ENOSYS;
}

int
pthread_setcancelstate(int state, int *oldstate)
{
	if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
		return EINVAL;
	if (oldstate != NULL)
		*oldstate = PTHREAD_CANCEL_DISABLE;
	return 0;
}

int
pthread_setcanceltype(int type, int *oldtype)
{
	if (type != PTHREAD_CANCEL_DEFERRED
	    && type != PTHREAD_CANCEL_ASYNCHRONOUS)
		return EINVAL;
	if (oldtype != NULL)
		*oldtype = PTHREAD_CANCEL_DEFERRED;
	return 0;
}

void
pthread_testcancel(void)
{
}

int
pthread_atfork(void (*prepare)(void), void (*parent)(void),
               void (*child)(void))
{
	/* There is no fork() on this platform, so a registered handler could
	 * never run; accept the registration and drop it. */
	(void)prepare;
	(void)parent;
	(void)child;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Cleanup handlers
 *
 * newlib's pthread_cleanup_push/pop macros expand to these.  Without
 * cancellation there is nothing that can run a handler behind the caller's
 * back, so the context only has to survive until the matching pop.
 * ------------------------------------------------------------------ */

void
_pthread_cleanup_push(struct _pthread_cleanup_context *context,
                      void (*routine)(void *), void *arg)
{
	context->_routine    = routine;
	context->_arg        = arg;
	context->_canceltype = 0;
	context->_previous   = NULL;
}

void
_pthread_cleanup_pop(struct _pthread_cleanup_context *context, int execute)
{
	if (execute && context->_routine != NULL)
		context->_routine(context->_arg);
}

void
_pthread_cleanup_push_defer(struct _pthread_cleanup_context *context,
                            void (*routine)(void *), void *arg)
{
	_pthread_cleanup_push(context, routine, arg);
}

void
_pthread_cleanup_pop_restore(struct _pthread_cleanup_context *context,
                             int execute)
{
	_pthread_cleanup_pop(context, execute);
}
