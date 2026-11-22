/* lv2_thread_tls.h - does the calling thread have a TLS area?
 *
 * Shared by librt (pthread.c, executables) and the PRX thread runtime
 * (runtime/lv2/crt/lv2-prx-thread.c, modules).  Internal: not installed.
 *
 * A thread made by liblv2's sys_ppu_thread_create (and so by
 * sysThreadCreate, pthread_create and the SDK's sys_ppu_thread_create) gets
 * a TLS area, and r13 is its thread pointer: 0x7000 bytes past the start
 * of the thread's TLS block, as the PowerPC 64-bit ELF ABI places it.  A
 * thread the kernel creates on its own has none: the thread that runs a
 * module's module_start / module_stop when Cobra loads a plugin into VSH
 * is one.
 * There r13 does not point at a TLS area, so anything that reads TLS
 * (librt's exit cleanup, liblv2's sys_ppu_thread_exit) faults.
 *
 * The check reads r13 itself and nothing in memory, so it is safe on such
 * a thread.  A thread pointer is at least 0x7000 and, in the 32-bit Lv-2
 * user address space, below 4 GiB.
 */
#ifndef PS3DK_LIBRT_LV2_THREAD_TLS_H
#define PS3DK_LIBRT_LV2_THREAD_TLS_H

#include <stdint.h>
#include <sys/lv2_syscall.h>

#define PT_TLS_POINTER_MIN 0x7000ull
#define PT_TLS_POINTER_MAX 0xffffffffull

/* The calling thread's r13, read without touching memory. */
static inline uint64_t
pt_tls_pointer(void)
{
	uint64_t tp;

	__asm__ __volatile__("mr %0, 13" : "=r"(tp));
	return tp;
}

/* Nonzero when r13 can be a TLS pointer. */
static inline int
pt_thread_has_tls(void)
{
	uint64_t tp = pt_tls_pointer();

	return tp >= PT_TLS_POINTER_MIN && tp <= PT_TLS_POINTER_MAX;
}

/* Ends the calling thread with Lv-2 system call 41 (_sys_ppu_thread_exit)
 * and nothing else: no librt cleanup and none of liblv2's, both of which
 * read TLS.  The exit code reaches sys_ppu_thread_join as usual. */
static inline void __attribute__((noreturn))
pt_raw_thread_exit(uint64_t exit_code)
{
	lv2syscall1(41, exit_code);
	__builtin_unreachable();
}

#endif
