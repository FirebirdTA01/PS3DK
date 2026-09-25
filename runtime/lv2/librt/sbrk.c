/*
 * sbrk.c — newlib sbrk backing store via a single up-front Lv-2 heap
 * arena (sys_memory_allocate / sys_memory_free).
 *
 * This is a correct-by-construction fixed-arena bump allocator: one
 * contiguous allocation at init, pure userland pointer arithmetic in
 * the successful data path. No incremental regions; the first exhaustion
 * writes a diagnostic directly to Lv-2 without allocating.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/reent.h>
#include <sys/types.h>
/* The installed TTY header still imports legacy ppu-lv2 macros. Import it
 * first so sys/lv2_syscall.h's first-include-wins guard avoids redefinitions. */
#include <sys/tty.h>
#include <sys/memory.h>
#include <sys/lv2errno.h>
#include <sys/syscalls.h>
#include <sys/heap_config.h>
#include <sys/process.h>

/* ------------------------------------------------------------------ *
 * Tunable — conservative default, safe for all current samples.
 * ------------------------------------------------------------------ */

#define SBRK_PAGE_SIZE UINT64_C(1048576)
const uint64_t __ps3tc_heap_size __attribute__((weak)) = UINT64_C(64) * 1048576;

/* ------------------------------------------------------------------ *
 * Arena state — protected by newlib's internal malloc lock.
 * No additional Lv-2 spinlock is needed in the bump path.
 * ------------------------------------------------------------------ */

static int            __sbrk_ready;          /* 1 after successful init */
static char          *__sbrk_base;           /* arena start */
static char          *__sbrk_end;            /* arena end (base+capacity) */
static char          *__sbrk_cur;            /* current bump pointer */
static uint32_t       __sbrk_capacity;
static int            __sbrk_exhaustion_reported;

/* Startup has no allocator yet; sbrk failure runs under malloc's lock.
 * Neither path may call printf/snprintf/write or allocate memory. */
static char heap_message[256];
static unsigned heap_message_length;

static void heap_text(const char *text)
{
	while (*text && heap_message_length < sizeof(heap_message) - 1)
		heap_message[heap_message_length++] = *text++;
}

static void heap_number(uint64_t value)
{
	char digits[20];
	unsigned count = 0;
	do {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	while (count && heap_message_length < sizeof(heap_message) - 1)
		heap_message[heap_message_length++] = digits[--count];
}

static void heap_write_message(void)
{
	u32 written;
	heap_text("\n");
	(void)sysTtyWrite(2, heap_message, heap_message_length, &written);
}

static void __attribute__((noreturn))
heap_startup_failure(uint64_t requested, uint64_t rounded, uint32_t available,
                     uint32_t error, const char *reason)
{
	static const char hex[] = "0123456789abcdef";
	heap_message_length = 0;
	heap_text("PS3TC heap: startup failure requested="); heap_number(requested);
	heap_text(" rounded="); heap_number(rounded);
	heap_text(" available="); heap_number(available);
	heap_text(" error=0x");
	for (int shift = 28; shift >= 0; shift -= 4)
		heap_message[heap_message_length++] = hex[(error >> shift) & 15];
	heap_text(" reason="); heap_text(reason);
	heap_write_message();
	/* The syscall table is not wired until constructor 104. Exit directly. */
	sysProcessExit(1);
	__builtin_trap();
}

static void heap_exhausted(ptrdiff_t increment)
{
	if (__sbrk_exhaustion_reported) return;
	__sbrk_exhaustion_reported = 1;
	heap_message_length = 0;
	heap_text("PS3TC heap: exhausted increment=");
	if (increment < 0) {
		heap_text("-");
		heap_number((uint64_t)(-(increment + 1)) + 1);
	} else {
		heap_number((uint64_t)increment);
	}
	heap_text(" break="); heap_number((uint64_t)(__sbrk_cur - __sbrk_base));
	heap_text(" capacity="); heap_number(__sbrk_capacity);
	heap_write_message();
}

/* ------------------------------------------------------------------ *
 * Init / deinit
 * ------------------------------------------------------------------ */

static void sbrk_init(void) __attribute__((constructor(103)));
static void
sbrk_init(void)
{
	sys_mem_addr_t addr;
	sys_memory_info_t memory;
	/* Read the link-selected definition, not a folded weak initializer.
	 * GCC can otherwise fold this translation unit's const default at -O2. */
	uint64_t requested = *(const volatile uint64_t *)&__ps3tc_heap_size;
	if (!requested || requested > UINT64_MAX - (SBRK_PAGE_SIZE - 1))
		heap_startup_failure(requested, 0, 0, EINVAL, "invalid-size");
	uint64_t rounded = (requested + SBRK_PAGE_SIZE - 1) & ~(SBRK_PAGE_SIZE - 1);
	if (rounded > UINT32_MAX || rounded > (uint64_t)PTRDIFF_MAX)
		heap_startup_failure(requested, rounded, 0, EINVAL, "size-out-of-range");
	s32 ret = sys_memory_get_user_memory_size(&memory);
	if (ret)
		heap_startup_failure(requested, rounded, 0, (uint32_t)ret, "memory-query");
	if (rounded > memory.available_user_memory)
		heap_startup_failure(requested, rounded, memory.available_user_memory,
		                     ENOMEM, "insufficient-memory");
	ret = sys_memory_allocate((uint32_t)rounded,
		SYS_MEMORY_PAGE_SIZE_1M,
		&addr);
	if (ret)
		heap_startup_failure(requested, rounded, memory.available_user_memory,
		                     (uint32_t)ret, "allocation");
	if ((uintptr_t)addr > UINTPTR_MAX - (uintptr_t)rounded)
		heap_startup_failure(requested, rounded, memory.available_user_memory,
		                     EINVAL, "address-overflow");

	__sbrk_base  = (char *)(uintptr_t)addr;
	__sbrk_end   = __sbrk_base + (size_t)rounded;
	__sbrk_cur   = __sbrk_base;
	__sbrk_capacity = (uint32_t)rounded;
	__sbrk_ready = 1;
}

/* Other PPU threads can still use malloc storage while the exiting thread
 * runs finalizers. Keep the process-owned arena mapped until Lv-2 tears
 * down the process; no .fini callback may free it early. */

/* ------------------------------------------------------------------ *
 * sbrk — newlib backing store (pure bump pointer)
 * ------------------------------------------------------------------ */

caddr_t
__librt_sbrk_r(struct _reent *ptr, ptrdiff_t incr)
{
	if (!__sbrk_ready) {
		ptr->_errno = ENOMEM;
		return (caddr_t)-1;
	}

	if (incr >= 0) {
		if ((uint64_t)incr > (uint64_t)(__sbrk_end - __sbrk_cur)) {
			/* Check distances before forming any potentially invalid pointer. */
			ptr->_errno = ENOMEM;
			heap_exhausted(incr);
			return (caddr_t)-1;
		}
		char *old = __sbrk_cur;
		__sbrk_cur += incr;
		return (caddr_t)old;
	} else {
		/* incr < 0: shrink */
		uint64_t decrease = (uint64_t)(-(incr + 1)) + 1;
		uint64_t used = (uint64_t)(__sbrk_cur - __sbrk_base);
		__sbrk_cur = decrease > used ? __sbrk_base : __sbrk_cur - (size_t)decrease;
		return (caddr_t)__sbrk_cur;
	}
}
