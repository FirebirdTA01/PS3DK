/*
 * lv2-prx-thread.c - sys_ppu_thread_create / sys_ppu_thread_exit for a
 * relocatable module (PRX).
 *
 * <sys/ppu_thread.h> routes the two calls to __lv2_ppu_thread_create and
 * __lv2_thread_exit.  In an executable librt provides them (pthread.c),
 * with the exit cleanup for thread_locals and pthread keys.  That object
 * cannot go into a module: its per-thread state is TLS, and a module has
 * no TLS of its own (prx-gen refuses TLS relocations), so linking it fails.
 * A module's threads also include ones with no TLS area at all: Cobra runs
 * a VSH plugin's module_start and module_stop on a thread the kernel
 * creates.
 *
 * This file is the module-side pair.  It reads no TLS and allocates
 * nothing: sys_ppu_thread_create is safe on such a thread, and
 * sys_ppu_thread_exit ends it with the bare exit system call.  It is built
 * per ABI into liblv2prx.a, which lv2-prx.specs puts ahead of librt, so a
 * module's references resolve here and librt's pthread.o never enters the
 * link.  A module gets no thread_local or pthread key cleanup (it cannot
 * use either: both are TLS).
 */

#include <stddef.h>
#include <stdint.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <lv2/thread.h>

#include "../librt/lv2_thread_tls.h"

void __lv2_thread_exit(uint64_t exit_code) __attribute__((noreturn));
int __lv2_ppu_thread_create(sys_ppu_thread_t *thread_id,
                            void (*entry)(uint64_t), uint64_t arg, int prio,
                            size_t stacksize, uint64_t flags,
                            const char *threadname);

#define PRX_SYS_PPU_THREAD_CREATE_INTERRUPT 0x2ull
#define PRX_LV2_EAGAIN ((s32)0x80010001)

/* A thread with a TLS area (made by liblv2's sys_ppu_thread_create, which
 * is every thread sys_ppu_thread_create starts) ends through liblv2, as
 * before; one without (a kernel-made thread) ends with system call 41. */
void
__lv2_thread_exit(uint64_t exit_code)
{
	if (!pt_thread_has_tls())
		pt_raw_thread_exit(exit_code);
	sysThreadExit(exit_code);
	__builtin_unreachable();
}

/* The entry and its 64-bit argument reach the new thread through a start
 * record: sysThreadCreate passes one pointer.  Records are a fixed pool
 * rather than heap memory, because the creating thread may be one with no
 * TLS, where newlib's allocator must not run.  The new thread releases
 * its record before it calls the entry, so the pool bounds only threads
 * that have been created and have not started yet. */
#define PRX_START_RECORDS 16

struct prx_start {
	uint32_t   busy;
	void     (*entry)(uint64_t);
	uint64_t   arg;
};

static struct prx_start prx_start_records[PRX_START_RECORDS];

static struct prx_start *
prx_start_claim(void)
{
	unsigned int i;

	for (i = 0; i < PRX_START_RECORDS; i++) {
		uint32_t idle = 0;

		if (__atomic_compare_exchange_n(&prx_start_records[i].busy, &idle, 1,
		                                0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return &prx_start_records[i];
	}
	return NULL;
}

static void
prx_start_release(struct prx_start *start)
{
	__atomic_store_n(&start->busy, 0, __ATOMIC_RELEASE);
}

/* A return from the entry ends the thread with exit code 0, as the
 * executable's sys_ppu_thread_create does. */
static void
prx_thread_trampoline(void *p)
{
	struct prx_start *start = (struct prx_start *)p;
	void (*entry)(uint64_t) = start->entry;
	uint64_t arg = start->arg;

	prx_start_release(start);
	entry(arg);
	__lv2_thread_exit(0);
}

/* As in an executable: interrupt threads are created as they are (they
 * never exit through here), and the full 64-bit argument reaches the
 * entry.  With every start record in use the call fails with EAGAIN. */
int
__lv2_ppu_thread_create(sys_ppu_thread_t *thread_id,
                        void (*entry)(uint64_t), uint64_t arg, int prio,
                        size_t stacksize, uint64_t flags,
                        const char *threadname)
{
	struct prx_start *start;
	s32 rc;

	if (flags & PRX_SYS_PPU_THREAD_CREATE_INTERRUPT)
		return (int)sysThreadCreate(thread_id,
		                            (void (*)(void *))(void (*)(void))entry,
		                            (void *)(uintptr_t)arg, (s32)prio,
		                            (u64)stacksize, flags,
		                            (char *)threadname);
	start = prx_start_claim();
	if (start == NULL)
		return (int)PRX_LV2_EAGAIN;
	start->entry = entry;
	start->arg   = arg;
	rc = sysThreadCreate(thread_id, prx_thread_trampoline, start, (s32)prio,
	                     (u64)stacksize, flags, (char *)threadname);
	if (rc != 0)
		prx_start_release(start);
	return (int)rc;
}
