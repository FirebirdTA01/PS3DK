/*
 * sbrk.c — newlib sbrk backing store via a single up-front Lv-2 heap
 * arena (sysMemoryAllocate / sysMemoryFree).
 *
 * This is a correct-by-construction fixed-arena bump allocator: one
 * contiguous allocation at init, pure userland pointer arithmetic in
 * the data path.  No incremental regions, no syscalls in the sbrk
 * hot path, no spinlock held across any kernel call.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/reent.h>
#include <sys/types.h>
#include <sys/memory.h>
#include <sys/lv2errno.h>
#include <sys/syscalls.h>

/* ------------------------------------------------------------------ *
 * Tunable — conservative default, safe for all current samples.
 * ------------------------------------------------------------------ */

#define SBRK_HEAP_SIZE  (64 * 1024 * 1024)   /* 64 MiB — TUNABLE */

/* ------------------------------------------------------------------ *
 * Arena state — protected by newlib's internal malloc lock.
 * No Lv-2 spinlock needed (no syscalls in the bump path).
 * ------------------------------------------------------------------ */

static int            __sbrk_ready;          /* 1 after successful init */
static char          *__sbrk_base;           /* arena start */
static char          *__sbrk_end;            /* arena end (base+HEAP_SIZE) */
static char          *__sbrk_cur;            /* current bump pointer */

/* ------------------------------------------------------------------ *
 * Init / deinit
 * ------------------------------------------------------------------ */

static void sbrk_init(void) __attribute__((constructor(103)));
static void
sbrk_init(void)
{
	sys_mem_addr_t addr;

	s32 ret = sysMemoryAllocate(SBRK_HEAP_SIZE,
		SYS_MEMORY_PAGE_SIZE_1M,
		&addr);
	if (ret) {
		__sbrk_ready = 0;
		return;
	}

	__sbrk_base  = (char *)(uintptr_t)addr;
	__sbrk_end   = __sbrk_base + SBRK_HEAP_SIZE;
	__sbrk_cur   = __sbrk_base;
	__sbrk_ready = 1;
}

/*
 * sbrk_deinit() runs in .fini so it executes after
 * __deregister_frame_info / __do_global_dtors_aux.
 */
asm ("\t.section\t.fini\n\tbl sbrk_deinit\n\tnop\n\t.previous");

static void __attribute__((used))
sbrk_deinit(void)
{
	if (__sbrk_ready)
		sysMemoryFree((sys_mem_addr_t)(uintptr_t)__sbrk_base);
	__sbrk_ready = 0;
}

/* ------------------------------------------------------------------ *
 * sbrk — newlib backing store (pure bump pointer)
 * ------------------------------------------------------------------ */

caddr_t
__librt_sbrk_r(struct _reent *ptr, ptrdiff_t incr)
{
	if (!__sbrk_ready) {
		lv2errno_r(ptr, ENOMEM);
		return (caddr_t)-1;
	}

	if (incr >= 0) {
		if (__sbrk_cur + incr > __sbrk_end || __sbrk_cur + incr < __sbrk_cur) {
			/* overflow or out-of-space */
			lv2errno_r(ptr, ENOMEM);
			return (caddr_t)-1;
		}
		char *old = __sbrk_cur;
		__sbrk_cur += incr;
		return (caddr_t)old;
	} else {
		/* incr < 0: shrink */
		char *new_cur = __sbrk_cur + incr;
		if (new_cur < __sbrk_base)
			new_cur = __sbrk_base;
		__sbrk_cur = new_cur;
		return (caddr_t)__sbrk_cur;
	}
}
