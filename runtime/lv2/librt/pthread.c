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
 *     is exactly the POSIX semantics (no waiters).  Lv-2 also refuses to
 *     destroy a mutex while a condition variable bound to it exists, while
 *     POSIX lets a program destroy the two in either order; so every bound
 *     cond is recorded, and pthread_mutex_destroy destroys the idle ones
 *     first and returns them to "not created" (see "Cond bindings").
 *
 *   * Lv-2 has no thread-specific-data.  Each thread's values live in its
 *     slot, found through a per-thread TLS cookie (never the Lv-2 thread id,
 *     which Lv-2 reuses).  Slots come in chunks that grow on demand and
 *     never move; keys are bounded (PT_KEYS_MAX), and pthread_key_create
 *     reports EAGAIN when they run out rather than corrupting anything.
 *     Thread exit runs thread_local destructors, then key destructors (see
 *     "Thread exit").
 *
 * Deliberately NOT implemented, because Lv-2 offers nothing to build them
 * on: cancellation (pthread_cancel returns ENOSYS; setcancelstate /
 * setcanceltype accept and remember nothing), and stack unwinding through
 * pthread_exit (cleanup handlers pushed with pthread_cleanup_push run only
 * when popped with a non-zero argument).
 *
 * Errno note: on this target the E* names are the Lv-2 status codes
 * (EBUSY == 0x8001000A, see <sys/_lv2_errno.h>), so a pthread function
 * returns the same value a caller compares against by name, and lv2error()
 * from <sys/lv2errno.h> maps a status code to itself.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* Thread slots come in chunks of PT_CHUNK_SLOTS: the first chunk is static,
 * later ones are allocated when needed and never freed or moved, so a slot's
 * address stays valid for as long as anything holds it (a TLS cookie, a
 * trampoline argument, a blocked pthread_join).  Keys that can exist at one
 * time: PT_KEYS_MAX.  cairo needs no keys at all and pixman needs exactly
 * one. */
#define PT_CHUNK_SLOTS   32
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
/* Held only while pthread_mutex_destroy unbinds the cond (under
 * pt_table_mutex): signal and broadcast treat it as "no waiters", a wait
 * goes to the table lock and finds the outcome. */
#define PT_C_UNBINDING    0xFFFFFFFEu

/* CELL_EDEADLK.  Needed by name in pthread_mutex_trylock, where the POSIX
 * answer differs from the straight lv2error() translation. */
#define PT_LV2_EDEADLK    ((s32)0x80010008)

/* CELL_EPERM: what sys_rwlock_wunlock returns to a thread that does not hold
 * the write lock (pthread_rwlock_unlock uses it to tell the two holds apart). */
#define PT_LV2_EPERM      ((s32)0x80010009)

/* pthread_rwlock_t sentinels: 0xFFFFFFFF is newlib's
 * _PTHREAD_RWLOCK_INITIALIZER, and a zeroed lock counts as not created, as
 * for mutexes. */
#define PT_RW_UNINIT      0x00000000u
#define PT_RW_STATIC      0xFFFFFFFFu

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

/* pt_err for the rwlock try-locks.  POSIX gives them one way to fail on a
 * held lock, EBUSY, whoever holds it; Lv-2 reports a lock the calling thread
 * already holds for writing as a self-deadlock (EDEADLK), which a call that
 * never waits cannot be. */
static int
pt_try_err(s32 rc)
{
	int err = pt_err(rc);

	return err == EDEADLK ? EBUSY : err;
}

/* ------------------------------------------------------------------ *
 * Process-wide locks
 *
 * pt_table_mutex serialises every table mutation and every lazy creation.
 * It is only ever held across syscalls, never across user code, so it can
 * not be held for an unbounded time.
 *
 * It is recursive so that a destructor calling back in does not
 * self-deadlock.  pthread_once runs user code, so it takes no process-wide
 * lock at all: each control carries its own state (see pthread_once).
 * ------------------------------------------------------------------ */

#define PT_BOOT_UNSTARTED  0u
#define PT_BOOT_RUNNING    1u
#define PT_BOOT_READY      2u
#define PT_BOOT_FAILED     3u

static unsigned int pt_boot_state;
static sys_mutex_t  pt_table_mutex;

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

/* Create the process-wide table lock exactly once.  The winner of the
 * compare-and-swap creates it; everyone else sleeps until the state settles.
 * Sleeping (sysUsleep, a plain syscall that needs nothing from this file),
 * not yielding: a yield only hands the processor to threads of equal or
 * higher priority, so higher-priority losers could keep a lower-priority
 * winner from ever finishing. */

/* Test hooks, empty in the library: PT_TEST_BOOT_CLAIMED runs in the winner
 * after it wins the claim, PT_TEST_BOOT_WAITING in each pass of a loser's
 * sleep (the pthread-bootstrap-contested row). */
#ifndef PT_TEST_BOOT_CLAIMED
#define PT_TEST_BOOT_CLAIMED() ((void)0)
#endif
#ifndef PT_TEST_BOOT_WAITING
#define PT_TEST_BOOT_WAITING() ((void)0)
#endif
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

		PT_TEST_BOOT_CLAIMED();
		pt_mutex_attr_init(&attr, 1);
		rc = sysMutexCreate(&pt_table_mutex, &attr);
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
		PT_TEST_BOOT_WAITING();
		(void)sysUsleep(50);
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
	unsigned char    cleanup;    /* 0 not started, 1 running, 2 done */
	pthread_t        handle;     /* fixed for the life of the slot */
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

struct pt_chunk {
	struct pt_chunk  *next;
	struct pt_thread  slot[PT_CHUNK_SLOTS];
};

static struct pt_chunk  pt_chunk0;
static struct pt_chunk *pt_chunk_last = &pt_chunk0;
static unsigned int     pt_nchunks;	/* 0 until pt_chunk0 is set up */
static struct pt_key    pt_keys[PT_KEYS_MAX];

/* The calling thread's slot, set when the thread first gets one (in its
 * pthread_create trampoline, or when a thread not made by pthread_create
 * first needs one) and cleared by its own cleanup.  Identity comes only from
 * here, never from the Lv-2 thread id, which Lv-2 reuses. */
static __thread struct pt_thread *pt_self_cookie;

/* Caller holds the table lock. */
static void
pt_chunk_init(struct pt_chunk *c, unsigned int first_handle)
{
	unsigned int i;

	for (i = 0; i < PT_CHUNK_SLOTS; i++)
		c->slot[i].handle = (pthread_t)(first_handle + i);
}

#define PT_FOR_EACH_SLOT(c, t)                                          \
	for ((c) = (pt_nchunks ? &pt_chunk0 : NULL); (c) != NULL;       \
	     (c) = (c)->next)                                           \
		for ((t) = (c)->slot; (t) < (c)->slot + PT_CHUNK_SLOTS; (t)++)

/* A pthread_t is a 1-based slot number across the chunks, never a raw Lv-2
 * id: sys_ppu_thread_t is 64 bits and pthread_t is 32, so the raw id does
 * not fit.  0 is therefore always an invalid pthread_t.  Caller holds the
 * table lock. */
static struct pt_thread *
pt_slot_from_handle(pthread_t handle)
{
	unsigned int idx = (unsigned int)handle;
	struct pt_chunk *c = &pt_chunk0;
	unsigned int n;

	if (idx == 0 || pt_nchunks == 0)
		return NULL;
	idx--;
	for (n = idx / PT_CHUNK_SLOTS; n > 0 && c != NULL; n--)
		c = c->next;
	if (c == NULL || !c->slot[idx % PT_CHUNK_SLOTS].used)
		return NULL;
	return &c->slot[idx % PT_CHUNK_SLOTS];
}

static pthread_t
pt_handle_from_slot(const struct pt_thread *slot)
{
	return slot->handle;
}

/* Caller holds the table lock.  A free slot, cleared but keeping its handle;
 * a new chunk when every slot is taken (EAGAIN if that allocation fails). */
static int
pt_slot_alloc_locked(struct pt_thread **out)
{
	struct pt_chunk *c;
	struct pt_thread *t;

	if (pt_nchunks == 0) {
		pt_chunk_init(&pt_chunk0, 1);
		pt_nchunks = 1;
	}
	PT_FOR_EACH_SLOT(c, t) {
		if (!t->used) {
			pthread_t handle = t->handle;

			memset(t, 0, sizeof(*t));
			t->handle = handle;
			*out = t;
			return 0;
		}
	}
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return EAGAIN;
	pt_chunk_init(c, pt_nchunks * PT_CHUNK_SLOTS + 1);
	pt_chunk_last->next = c;
	pt_chunk_last = c;
	pt_nchunks++;
	*out = &c->slot[0];
	return 0;
}

/* Caller holds the table lock.  The calling thread's slot; a thread not
 * created through pthread_create (the main thread, or one from a raw
 * sys_ppu_thread_create) gets a detached slot on first use, so that nothing
 * ever waits to join it.  Such a slot is released by the thread's cleanup
 * when it exits through a covered route (pt_thread_cleanup); a thread that
 * exits any other way leaves it allocated (one slot per such thread). */
static int
pt_self_slot_locked(struct pt_thread **out)
{
	struct pt_thread *t = pt_self_cookie;
	sys_ppu_thread_t id = 0;
	int err;

	if (t != NULL && t->used) {
		*out = t;
		return 0;
	}
	if (sysThreadGetId(&id) != 0)
		return ESRCH;
	err = pt_slot_alloc_locked(&t);
	if (err != 0)
		return err;
	t->used     = 1;
	t->foreign  = 1;
	t->detached = 1;
	t->lv2_id   = id;
	pt_self_cookie = t;
	*out = t;
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

/* ------------------------------------------------------------------ *
 * Thread exit
 *
 * thread_local destructors registered through __cxa_thread_atexit_impl
 * (libstdc++ uses it for every thread_local with a non-trivial destructor)
 * run first, then the thread-specific-data destructors, where libstdc++'s
 * notify_all_at_thread_exit and promise::set_value_at_thread_exit run: so
 * a thread's thread_locals are destroyed before it notifies anyone, whatever
 * order the program registered things in.
 * ------------------------------------------------------------------ */

struct pt_tls_dtor {
	void              (*fn)(void *);
	void               *obj;
	struct pt_tls_dtor *next;
};

static __thread struct pt_tls_dtor *pt_tls_dtors;

/* Test hook, empty in the library: the allocation behind
 * __cxa_thread_atexit_impl (the thread-atexit-oom row makes it fail). */
#ifndef PT_TEST_TLS_ALLOC
#define PT_TEST_TLS_ALLOC(size) malloc(size)
#endif

/* Registers a thread_local's destructor for the calling thread.  There is no
 * dynamic loading on Lv-2, so dso_handle is not needed to keep a module
 * loaded.  Failure is fatal (as in glibc): libstdc++'s __cxa_thread_atexit
 * returns the result to code that ignores it, and a destructor silently
 * dropped is worse than stopping.  The message is written without any
 * allocation. */
int
__cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso_handle)
{
	struct pt_tls_dtor *d = PT_TEST_TLS_ALLOC(sizeof(*d));

	(void)dso_handle;
	if (d == NULL) {
		static const char msg[] =
			"librt: out of memory registering a thread_local destructor\n";
		(void)write(2, msg, sizeof(msg) - 1);
		abort();
	}
	d->fn = fn;
	d->obj = obj;
	d->next = pt_tls_dtors;
	pt_tls_dtors = d;
	return 0;
}

/* Runs the calling thread's thread_local destructors, last registered first,
 * until none are left (a destructor may register more).  Called by every
 * covered thread exit, and by newlib's exit() for the thread that calls it
 * before the atexit handlers (newlib patch 0019). */
void
__lv2_tls_dtors_run(void)
{
	struct pt_tls_dtor *d;

	while ((d = pt_tls_dtors) != NULL) {
		pt_tls_dtors = d->next;
		d->fn(d->obj);
		free(d);
	}
}

/* The one cleanup routine, run on the exiting thread by every covered exit
 * route: pthread_create's trampoline, pthread_exit, and the SDK's
 * sys_ppu_thread_exit / the trampoline behind sys_ppu_thread_create.  SELF
 * is the thread's slot, or NULL for a thread that never had one (its
 * thread_locals still die).  A joinable slot keeps its return value and is
 * released by pthread_join or pthread_detach; a detached one is released
 * here.  Re-entry (a destructor calling an exit route) does nothing more:
 * the raw exit that follows abandons the remaining callbacks, which is
 * outside what this guarantees.
 *
 * A thread with no slot yet can acquire one DURING the thread_local phase:
 * a destructor's first pthread_setspecific (or anything else that takes
 * the calling thread's slot) creates a detached slot.  The cleanup adopts
 * that slot after the phase, so its key destructors run and it is
 * released.  pt_in_cleanup guards re-entry on every path, including the
 * phase that runs before any slot exists. */
static __thread int pt_in_cleanup;

/* Mark SELF's cleanup as started; false if it already was. */
static int
pt_cleanup_claim(struct pt_thread *self)
{
	int first = 0;

	if (pt_table_lock() == 0) {
		first = self->cleanup == 0;
		if (first)
			self->cleanup = 1;
		pt_table_unlock();
	}
	return first;
}

static void
pt_thread_cleanup(struct pt_thread *self, void *retval)
{
	if (pt_in_cleanup)
		return;
	if (self != NULL && !pt_cleanup_claim(self))
		return;
	pt_in_cleanup = 1;

	__lv2_tls_dtors_run();

	if (self == NULL && pt_self_cookie != NULL && pt_cleanup_claim(pt_self_cookie))
		self = pt_self_cookie;

	if (self != NULL) {
		pt_run_tsd_destructors(self);
		if (pt_table_lock() == 0) {
			self->retval   = retval;
			self->finished = 1;
			self->cleanup  = 2;
			if (self->detached)
				self->used = 0;
			pt_table_unlock();
		}
	}
	/* pt_in_cleanup stays set: every caller exits the thread next, and an
	 * exit route reached after this point has nothing left to run. */
	pt_self_cookie = NULL;
}

static void
pt_trampoline(void *arg)
{
	struct pt_thread *self = (struct pt_thread *)arg;
	void *retval;

	pt_self_cookie = self;
	retval = self->start(self->arg);
	pt_thread_cleanup(self, retval);
	sysThreadExit(0);
}

/* The SDK's Cell-style API (sys/ppu_thread.h) routes here.  The PSL1GHT-level
 * sysThreadCreate / sysThreadExit stay raw syscall wrappers: pthread_create
 * uses them with its own trampoline, so nothing is wrapped twice. */
void
__lv2_thread_exit(uint64_t exit_code)
{
	pt_thread_cleanup(pt_self_cookie, NULL);
	sysThreadExit(exit_code);
	__builtin_unreachable();
}

struct pt_raw_start {
	void     (*entry)(uint64_t);
	uint64_t   arg;
};

static void
pt_raw_trampoline(void *p)
{
	struct pt_raw_start start = *(struct pt_raw_start *)p;

	free(p);
	start.entry(start.arg);
	__lv2_thread_exit(0);
}

#define PT_SYS_PPU_THREAD_CREATE_INTERRUPT 0x2ull
#define PT_LV2_ENOMEM ((s32)0x80010004)

/* sys_ppu_thread_create: the entry runs inside pt_raw_trampoline, so that a
 * return from it goes through the thread-exit cleanup (exit code 0, as
 * before).  Interrupt threads are created as they are: they never exit
 * through here.  The full 64-bit argument reaches the entry. */
int
__lv2_ppu_thread_create(sys_ppu_thread_t *thread_id,
                        void (*entry)(uint64_t), uint64_t arg, int prio,
                        size_t stacksize, uint64_t flags,
                        const char *threadname)
{
	struct pt_raw_start *start;
	s32 rc;

	if (flags & PT_SYS_PPU_THREAD_CREATE_INTERRUPT)
		return (int)sysThreadCreate(thread_id,
		                            (void (*)(void *))(void (*)(void))entry,
		                            (void *)(uintptr_t)arg, (s32)prio,
		                            (u64)stacksize, flags,
		                            (char *)threadname);
	start = malloc(sizeof(*start));
	if (start == NULL)
		return (int)PT_LV2_ENOMEM;
	start->entry = entry;
	start->arg   = arg;
	rc = sysThreadCreate(thread_id, pt_raw_trampoline, start, (s32)prio,
	                     (u64)stacksize, flags, (char *)threadname);
	if (rc != 0)
		free(start);
	return (int)rc;
}

/* ------------------------------------------------------------------ *
 * Mutexes
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Cond bindings
 *
 * Every Lv-2 condition variable this file creates, with the pthread_cond_t
 * that holds its id and the mutex it is bound to.  pthread_mutex_destroy
 * walks it to destroy the mutex's conds first (Lv-2 will not destroy a
 * mutex that still has one), and resets each such pthread_cond_t to "not
 * created", so a later wait creates a new one against whatever mutex it
 * then names.  The reset is a compare-and-swap against the recorded id:
 * a cond that was re-initialised in the meantime is left alone.  (A
 * program that frees a cond's storage without pthread_cond_destroy and
 * then destroys its mutex still has its old storage compared against the
 * id, which is the cost of POSIX's either-order destruction.)  Only
 * touched under pt_table_mutex.
 * ------------------------------------------------------------------ */

struct pt_cond_binding {
	pthread_cond_t *cond;
	sys_cond_t      id;
	sys_mutex_t     mutex;
};

static struct pt_cond_binding *pt_bindings;
static size_t pt_bindings_len, pt_bindings_cap;

/* Signals and broadcasts between loading a cond's id and their syscall.
 * pthread_cond_signal/broadcast do not need the mutex, so one may hold an
 * id that pthread_mutex_destroy is about to destroy: the unbinding first
 * replaces the public id with PT_C_UNBINDING (later loads see no id), then
 * waits for this count to reach zero (earlier loads have finished their
 * syscall), and only then destroys the kernel cond.  Signals are short
 * syscalls that never block, so the wait is brief; it sleeps rather than
 * yields so a lower-priority signaller still gets to finish. */
static unsigned int pt_cond_inflight;

/* Test hook, empty in the library: runs in signal and broadcast after the
 * id is loaded, before the syscall (the pthread-destroy-order row). */
#ifndef PT_TEST_COND_ID_LOADED
#define PT_TEST_COND_ID_LOADED() ((void)0)
#endif

static int
pt_binding_add_locked(pthread_cond_t *cond, sys_cond_t id, sys_mutex_t mutex)
{
	if (pt_bindings_len == pt_bindings_cap) {
		size_t cap = pt_bindings_cap ? 2 * pt_bindings_cap : 16;
		struct pt_cond_binding *grown =
			realloc(pt_bindings, cap * sizeof(*grown));

		if (grown == NULL)
			return ENOMEM;
		pt_bindings = grown;
		pt_bindings_cap = cap;
	}
	pt_bindings[pt_bindings_len].cond = cond;
	pt_bindings[pt_bindings_len].id = id;
	pt_bindings[pt_bindings_len].mutex = mutex;
	pt_bindings_len++;
	return 0;
}

static void
pt_binding_remove_at_locked(size_t i)
{
	pt_bindings[i] = pt_bindings[--pt_bindings_len];
}

static void
pt_binding_remove_locked(sys_cond_t id)
{
	for (size_t i = 0; i < pt_bindings_len; i++)
		if (pt_bindings[i].id == id) {
			pt_binding_remove_at_locked(i);
			return;
		}
}

/* Destroy every cond bound to MUTEX.  EBUSY, with the rest left bound, when
 * one still has waiters: POSIX makes destroying a mutex that a cond wait
 * refers to undefined, and EBUSY is the answer it suggests. */
static int
pt_unbind_mutex_locked(sys_mutex_t mutex)
{
	size_t i = 0;

	while (i < pt_bindings_len) {
		struct pt_cond_binding b = pt_bindings[i];
		unsigned int expected = (unsigned int)b.id;

		if (b.mutex != mutex) {
			i++;
			continue;
		}
		int published = __atomic_compare_exchange_n(
			b.cond, &expected, PT_C_UNBINDING, 0,
			__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

		while (__atomic_load_n(&pt_cond_inflight, __ATOMIC_SEQ_CST) != 0)
			(void)sysUsleep(10);
		if (sysCondDestroy(b.id) != 0) {
			if (published)
				__atomic_store_n(b.cond, (unsigned int)b.id,
				                 __ATOMIC_RELEASE);
			return EBUSY;
		}
		if (published)
			__atomic_store_n(b.cond, PT_C_UNINIT, __ATOMIC_RELEASE);
		pt_binding_remove_at_locked(i);
	}
	return 0;
}

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
	int err;

	if (mutex == NULL)
		return EINVAL;

	value = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
	if (pt_is_uncreated(value)) {
		/* Never used, so nothing was ever created. */
		__atomic_store_n(mutex, PT_M_UNINIT, __ATOMIC_RELEASE);
		return 0;
	}

	err = pt_table_lock();
	if (err != 0)
		return err;
	err = pt_unbind_mutex_locked((sys_mutex_t)value);
	if (err == 0) {
		rc = sysMutexDestroy((sys_mutex_t)value);
		err = pt_err(rc);
	}
	if (err == 0)
		__atomic_store_n(mutex, PT_M_UNINIT, __ATOMIC_RELEASE);
	pt_table_unlock();
	return err;
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

	/* Everything else translates normally (CELL_EBUSY is EBUSY). */
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

/* Lv-2 timeouts are microseconds, and 0 means "wait forever".  A deadline
 * already past, or less than a microsecond away, becomes the shortest wait
 * (1); one too far ahead to represent saturates at PT_TIMEOUT_MAX_USEC; the
 * rest round UP to whole microseconds, so a wait never ends early. */
#define PT_TIMEOUT_MAX_USEC ((u64)0x7FFFFFFFFFFFFFFFull)

static u64
pt_usec_until(s64 now_sec, s64 now_nsec, const struct timespec *abstime)
{
	s64 dsec, dnsec = (s64)abstime->tv_nsec - now_nsec, usec;

	if (__builtin_sub_overflow((s64)abstime->tv_sec, now_sec, &dsec))
		return (s64)abstime->tv_sec < now_sec ? 1 : PT_TIMEOUT_MAX_USEC;
	if (dsec < 0)
		return 1;
	if (dnsec < 0) {
		if (dsec == 0)
			return 1;
		dsec--;
		dnsec += 1000000000;
	}
	if (__builtin_mul_overflow(dsec, (s64)1000000, &usec) ||
	    __builtin_add_overflow(usec, (dnsec + 999) / 1000, &usec))
		return PT_TIMEOUT_MAX_USEC;
	return usec < 1 ? 1 : (u64)usec;
}

static int
pt_clock_known(clockid_t clock_id)
{
	return clock_id == CLOCK_REALTIME || clock_id == CLOCK_MONOTONIC;
}

/* The Lv-2 timeout for ABSTIME on CLOCK_ID: CLOCK_REALTIME, or
 * CLOCK_MONOTONIC on the counter clock_gettime(CLOCK_MONOTONIC) reads.
 * EINVAL for another clock, a NULL deadline, or nanoseconds outside
 * [0, 1000000000).  (libstdc++'s steady_clock waits come through here.)
 * If the real-time clock cannot be read, the wait is the shortest one. */
static int
pt_deadline_usec(clockid_t clock_id, const struct timespec *abstime, u64 *usec)
{
	if (!pt_clock_known(clock_id) || abstime == NULL ||
	    abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L)
		return EINVAL;
	if (clock_id == CLOCK_REALTIME) {
		u64 sec = 0, nsec = 0;

		if (sysGetCurrentTime(&sec, &nsec) != 0) {
			*usec = 1;
			return 0;
		}
		*usec = pt_usec_until((s64)sec, (s64)nsec, abstime);
	} else {
		u64 now = (u64)sysGetSystemTime();

		*usec = pt_usec_until((s64)(now / 1000000),
		                      (s64)(now % 1000000) * 1000, abstime);
	}
	return 0;
}

/* A timed lock that can be taken at once succeeds whatever the deadline
 * (POSIX: its validity need not be checked then), so the lock is tried
 * first and the deadline is validated only before waiting.  The clock is
 * checked first: an unknown clock is EINVAL either way. */
static int
pt_mutex_timed(pthread_mutex_t *mutex, clockid_t clock_id,
               const struct timespec *abstime)
{
	sys_mutex_t id;
	u64 usec;
	s32 rc;
	int err;

	if (!pt_clock_known(clock_id))
		return EINVAL;
	err = pt_mutex_resolve(mutex, &id);
	if (err != 0)
		return err;
	rc = sysMutexTryLock(id);
	if (rc == 0)
		return 0;
	err = pt_err(rc);
	if (err != EBUSY)
		return err;   /* EDEADLK: the caller holds this non-recursive mutex */
	err = pt_deadline_usec(clock_id, abstime, &usec);
	if (err != 0)
		return err;
	return pt_err(sysMutexLock(id, usec));
}

int
pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
	return pt_mutex_timed(mutex, CLOCK_REALTIME, abstime);
}

int
pthread_mutex_clocklock(pthread_mutex_t *mutex, clockid_t clock_id,
                        const struct timespec *abstime)
{
	return pt_mutex_timed(mutex, clock_id, abstime);
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
	int err;

	if (cond == NULL)
		return EINVAL;

	value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	if (value == PT_C_UNINIT || value == PT_C_STATIC) {
		__atomic_store_n(cond, PT_C_UNINIT, __ATOMIC_RELEASE);
		return 0;
	}

	err = pt_table_lock();
	if (err != 0)
		return err;
	/* Re-read: pthread_mutex_destroy may have destroyed it meanwhile. */
	value = __atomic_load_n(cond, __ATOMIC_RELAXED);
	if (value != PT_C_UNINIT && value != PT_C_STATIC) {
		rc = sysCondDestroy((sys_cond_t)value);
		err = pt_err(rc);
		if (err == 0)
			pt_binding_remove_locked((sys_cond_t)value);
	}
	if (err == 0)
		__atomic_store_n(cond, PT_C_UNINIT, __ATOMIC_RELEASE);
	pt_table_unlock();
	return err;
}

/* Resolve a pthread_cond_t against the mutex the caller is waiting with,
 * creating the Lv-2 condition variable on first wait.
 *
 * POSIX makes only CONCURRENT waits on different mutexes undefined: the
 * binding is dynamic and ends when the waits finish.  Here, as a known
 * limitation, the Lv-2 cond stays bound to the mutex of its first wait
 * until that mutex or the cond is destroyed; a later wait with another
 * mutex still waits on the first one's cond. */
static int
pt_cond_resolve(pthread_cond_t *cond, sys_mutex_t mutex_id, sys_cond_t *out)
{
	unsigned int value;
	int err;

	if (cond == NULL)
		return EINVAL;

	value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	if (value != PT_C_UNINIT && value != PT_C_STATIC &&
	    value != PT_C_UNBINDING) {
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
		err = pt_binding_add_locked(cond, id, mutex_id);
		if (err != 0) {
			(void)sysCondDestroy(id);
			pt_table_unlock();
			return err;
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
	u64 usec;
	int err = pt_deadline_usec(CLOCK_REALTIME, abstime, &usec);

	return err ? err : pt_cond_wait_common(cond, mutex, usec);
}

int
pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                       clockid_t clock_id, const struct timespec *abstime)
{
	u64 usec;
	int err = pt_deadline_usec(clock_id, abstime, &usec);

	return err ? err : pt_cond_wait_common(cond, mutex, usec);
}

/* Signal (ALL = 0) or broadcast a cond, counted in pt_cond_inflight from
 * before the id is loaded until after the syscall. */
static int
pt_cond_wake(pthread_cond_t *cond, int all)
{
	unsigned int value;
	s32 rc = 0;

	__atomic_add_fetch(&pt_cond_inflight, 1, __ATOMIC_SEQ_CST);
	value = __atomic_load_n(cond, __ATOMIC_SEQ_CST);
	/* Not created yet means nobody has ever waited on it, and POSIX makes
	 * signalling a condition with no waiters a no-op.  An unbinding cond
	 * is being destroyed with its mutex, which POSIX allows only when no
	 * wait refers to that mutex: none can be woken.  (If one does wait,
	 * the destroy is refused with EBUSY and this signal is dropped; that
	 * program's destroy was undefined.) */
	if (value != PT_C_UNINIT && value != PT_C_STATIC &&
	    value != PT_C_UNBINDING) {
		PT_TEST_COND_ID_LOADED();
		rc = all ? sysCondBroadcast((sys_cond_t)value)
		         : sysCondSignal((sys_cond_t)value);
	}
	__atomic_sub_fetch(&pt_cond_inflight, 1, __ATOMIC_SEQ_CST);
	return pt_err(rc);
}

int
pthread_cond_signal(pthread_cond_t *cond)
{
	if (cond == NULL)
		return EINVAL;

	return pt_cond_wake(cond, 0);
}

int
pthread_cond_broadcast(pthread_cond_t *cond)
{
	if (cond == NULL)
		return EINVAL;

	return pt_cond_wake(cond, 1);
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
	/* sysCondWait measures a relative timeout, which pt_deadline_usec
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
 * Read-write locks
 *
 * Over the Lv-2 rwlock (syscalls 120-125, 127, 148).  Like a mutex, a
 * statically initialised lock is created on first use under the table
 * lock.  Lv-2 releases a read hold and a write hold with different
 * syscalls, while pthread_rwlock_unlock does not say which one the caller
 * has: sys_rwlock_wunlock refuses (CELL_EPERM) a thread that is not the
 * writer, so unlock tries it first and otherwise releases a read hold.
 * ------------------------------------------------------------------ */

static int
pt_rw_is_uncreated(unsigned int value)
{
	return value == PT_RW_UNINIT || value == PT_RW_STATIC;
}

static int
pt_rwlock_create(sys_rwlock_t *id)
{
	sys_rwlock_attribute_t attr;

	memset(&attr, 0, sizeof(attr));
	sys_rwlock_attribute_initialize(attr);
	return pt_err(sys_rwlock_create(id, &attr));
}

static int
pt_rwlock_resolve(pthread_rwlock_t *rwlock, sys_rwlock_t *out)
{
	unsigned int value;
	int err;

	if (rwlock == NULL)
		return EINVAL;

	value = __atomic_load_n(rwlock, __ATOMIC_ACQUIRE);
	if (!pt_rw_is_uncreated(value)) {
		*out = (sys_rwlock_t)value;
		return 0;
	}

	err = pt_table_lock();
	if (err != 0)
		return err;
	value = __atomic_load_n(rwlock, __ATOMIC_RELAXED);
	if (pt_rw_is_uncreated(value)) {
		sys_rwlock_t id = 0;

		err = pt_rwlock_create(&id);
		if (err == 0) {
			value = (unsigned int)id;
			__atomic_store_n(rwlock, value, __ATOMIC_RELEASE);
		}
	}
	pt_table_unlock();
	if (err == 0)
		*out = (sys_rwlock_t)value;
	return err;
}

int
pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr)
{
	sys_rwlock_t id = 0;
	int err;

	(void)attr;
	if (rwlock == NULL)
		return EINVAL;
	err = pt_rwlock_create(&id);
	if (err == 0)
		__atomic_store_n(rwlock, (unsigned int)id, __ATOMIC_RELEASE);
	return err;
}

int
pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
	unsigned int value;
	int err;

	if (rwlock == NULL)
		return EINVAL;
	value = __atomic_load_n(rwlock, __ATOMIC_ACQUIRE);
	if (pt_rw_is_uncreated(value)) {
		__atomic_store_n(rwlock, PT_RW_UNINIT, __ATOMIC_RELEASE);
		return 0;
	}
	err = pt_err(sys_rwlock_destroy((sys_rwlock_t)value));
	if (err == 0)
		__atomic_store_n(rwlock, PT_RW_UNINIT, __ATOMIC_RELEASE);
	return err;
}

int
pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
	sys_rwlock_t id;
	int err = pt_rwlock_resolve(rwlock, &id);

	return err ? err : pt_err(sys_rwlock_rlock(id, 0));
}

int
pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
	sys_rwlock_t id;
	int err = pt_rwlock_resolve(rwlock, &id);

	return err ? err : pt_try_err(sys_rwlock_tryrlock(id));
}

/* The timed and clock rwlock acquisitions: try first, validate the deadline
 * only before waiting (as pt_mutex_timed).  A failed try other than EBUSY
 * (EDEADLK for a writer that already holds the lock) is returned as is. */
static int
pt_rwlock_timed(pthread_rwlock_t *rwlock, clockid_t clock_id,
                const struct timespec *abstime, int write)
{
	sys_rwlock_t id;
	u64 usec;
	int err;

	if (!pt_clock_known(clock_id))
		return EINVAL;
	err = pt_rwlock_resolve(rwlock, &id);
	if (err != 0)
		return err;
	err = pt_err(write ? sys_rwlock_trywlock(id) : sys_rwlock_tryrlock(id));
	if (err != EBUSY)
		return err;
	err = pt_deadline_usec(clock_id, abstime, &usec);
	if (err != 0)
		return err;
	return pt_err(write ? sys_rwlock_wlock(id, usec) : sys_rwlock_rlock(id, usec));
}

int
pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock,
                           const struct timespec *abstime)
{
	return pt_rwlock_timed(rwlock, CLOCK_REALTIME, abstime, 0);
}

int
pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
	sys_rwlock_t id;
	int err = pt_rwlock_resolve(rwlock, &id);

	return err ? err : pt_err(sys_rwlock_wlock(id, 0));
}

int
pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
	sys_rwlock_t id;
	int err = pt_rwlock_resolve(rwlock, &id);

	return err ? err : pt_try_err(sys_rwlock_trywlock(id));
}

int
pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock,
                           const struct timespec *abstime)
{
	return pt_rwlock_timed(rwlock, CLOCK_REALTIME, abstime, 1);
}

int
pthread_rwlock_clockrdlock(pthread_rwlock_t *rwlock, clockid_t clock_id,
                           const struct timespec *abstime)
{
	return pt_rwlock_timed(rwlock, clock_id, abstime, 0);
}

int
pthread_rwlock_clockwrlock(pthread_rwlock_t *rwlock, clockid_t clock_id,
                           const struct timespec *abstime)
{
	return pt_rwlock_timed(rwlock, clock_id, abstime, 1);
}

int
pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
	unsigned int value;
	s32 rc;

	if (rwlock == NULL)
		return EINVAL;
	value = __atomic_load_n(rwlock, __ATOMIC_ACQUIRE);
	if (pt_rw_is_uncreated(value))
		return EPERM;	/* never locked, so not held */
	rc = sys_rwlock_wunlock((sys_rwlock_t)value);
	if (rc == PT_LV2_EPERM)
		rc = sys_rwlock_runlock((sys_rwlock_t)value);
	return pt_err(rc);
}

int
pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	memset(attr, 0, sizeof(*attr));
	attr->is_initialized = 1;
	return 0;
}

int
pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	attr->is_initialized = 0;
	return 0;
}

int
pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared)
{
	if (attr == NULL)
		return EINVAL;
	/* Process-private (0) only, as for mutexes and conds. */
	return pshared == 0 ? 0 : ENOTSUP;
}

int
pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *pshared)
{
	if (attr == NULL || pshared == NULL)
		return EINVAL;
	*pshared = 0;
	return 0;
}

/* ------------------------------------------------------------------ *
 * One-time initialisation
 * ------------------------------------------------------------------ */

/* pthread_once_t.init_executed is the control's state: PT_ONCE_IDLE, then
 * PT_ONCE_RUNNING while one caller runs the routine, PT_ONCE_DONE once it
 * has returned.  Each control is independent: unrelated controls never wait
 * for each other, so a routine may start a thread and wait for it to run a
 * different control.  Waiters sleep, not yield, so a lower-priority runner
 * still gets the processor.
 *
 * std::call_once reaches here through __gthread_once, and its callable may
 * throw.  This file is built with -fexceptions, so the cleanup attribute on
 * `armed` runs while such an exception unwinds through pthread_once: it puts
 * the control back to idle and the next waiter runs the routine instead.
 * DONE is published only after the routine returned.  A routine that
 * re-enters its own control waits for itself, which POSIX leaves undefined. */
#define PT_ONCE_IDLE     0
#define PT_ONCE_RUNNING  1
#define PT_ONCE_DONE     2

/* Test hook, empty in the library: runs in a waiter each time it finds the
 * control running, before it sleeps (the cxx-call-once row). */
#ifndef PT_TEST_ONCE_WAITING
#define PT_TEST_ONCE_WAITING() ((void)0)
#endif

static void
pt_once_abandon(pthread_once_t **armed)
{
	if (*armed != NULL)
		__atomic_store_n(&(*armed)->init_executed, PT_ONCE_IDLE,
		                 __ATOMIC_RELEASE);
}

int
pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
	unsigned int sleep_us = 50;

	if (once_control == NULL || init_routine == NULL)
		return EINVAL;

	for (;;) {
		int state = __atomic_load_n(&once_control->init_executed,
		                            __ATOMIC_ACQUIRE);
		int expected = PT_ONCE_IDLE;

		if (state == PT_ONCE_DONE)
			return 0;
		if (state == PT_ONCE_IDLE &&
		    __atomic_compare_exchange_n(&once_control->init_executed,
		                                &expected, PT_ONCE_RUNNING, 0,
		                                __ATOMIC_ACQUIRE,
		                                __ATOMIC_RELAXED)) {
			pthread_once_t *armed
				__attribute__((cleanup(pt_once_abandon))) =
				once_control;

			init_routine();
			armed = NULL;
			__atomic_store_n(&once_control->init_executed,
			                 PT_ONCE_DONE, __ATOMIC_RELEASE);
			return 0;
		}
		PT_TEST_ONCE_WAITING();
		(void)sysUsleep(sleep_us);
		if (sleep_us < 1000)
			sleep_us *= 2;
	}
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
			struct pt_chunk *c;
			struct pt_thread *t;

			pt_keys[i].used       = 1;
			pt_keys[i].destructor = destructor;
			/* A key index can be reused after pthread_key_delete;
			 * POSIX requires the new key to start out NULL in every
			 * thread. */
			PT_FOR_EACH_SLOT(c, t)
				t->tsd[i] = NULL;
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

	err = pt_slot_alloc_locked(&slot);
	if (err != 0) {
		pt_table_unlock();
		return err;
	}
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
	/* A thread with no slot has no thread-specific data to clean up, but
	 * its thread_locals still die. */
	pt_thread_cleanup(pt_self_cookie, value_ptr);
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
