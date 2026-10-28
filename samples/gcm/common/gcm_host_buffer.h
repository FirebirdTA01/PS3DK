/*
 * samples/gcm/common/gcm_host_buffer.h
 *
 * Helper for the GCM/RSX host-buffer allocation.  Every GCM sample
 * (samples/gcm/*/ and the PSGL render samples under samples/toolchain/)
 * currently does:
 *
 *     host = memalign(1024*1024, HOST_SIZE);   // 1 MiB-aligned, 1-32 MiB
 *     ... cellGcmInit(cmdsize, HOST_SIZE, host); ...
 *     free(host);
 *
 * The buffer comes out of the malloc heap and the PS3 userland
 * sbrk arena is a fixed 64 MiB pool shared with everything else.
 * A 32 MiB memalign() call is enough to burn the arena; the only
 * way an app can still allocate a framebuffer after the pattern was
 * to grab its own before the sample did.  (The EMP engine lost 32 MiB
 * this way.)
 *
 * Instead, take the buffer straight from LV2: sys_memory_allocate()
 * with SYS_MEMORY_PAGE_SIZE_1M pages backs the allocation without
 * consuming the sbrk arena.  The caller still passes the same size to
 * cellGcmInit() / rsxInit() so the ring still fits in the buffer.
 *
 * Usage:
 *
 *     #include "../../common/gcm_host_buffer.h"
 *
 *     sys_addr_t host = 0;
 *     rc = gcm_host_buffer_alloc(HOST_SIZE, &host);
 *     if (rc != CELL_OK) { ... return rc; }
 *     ...  cellGcmInit(CB_SIZE, HOST_SIZE, host); ...
 *     gcm_host_buffer_free(host);
 *
 * Both functions are static inline so the header is header-only and
 * links with both ILP32 and LP64.  Error propagation is the caller's
 * responsibility (return-code check); the helper never returns a
 * spurious CELL_OK on failure.
 */

#ifndef __PS3TC_SAMPLES_GCM_COMMON_HOST_BUFFER_H__
#define __PS3TC_SAMPLES_GCM_COMMON_HOST_BUFFER_H__

#include <sys/memory.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocate `size` bytes as 1 MiB-backed LV2 memory pages.  `size` is
 * expected to be a multiple of the 1 MiB page size, which holds the
 * sizes every current GCM sample passes (1 MiB, 16 MiB, 32 MiB).
 * Returns CELL_OK and stores the buffer's start address in *addr on
 * success; returns the LV2 error code from sys_memory_allocate()
 * otherwise (and leaves *addr = 0).  Passing addr == 0 is a caller
 * programming error: it returns a non-OK error code (0x80000001) rather
 * than dereferencing the null pointer.
 */
static inline int gcm_host_buffer_alloc(uint32_t size, sys_addr_t *addr)
{
    if (addr == 0)
        return CELL_ERROR_MAKE_ERROR(CELL_ERROR_FACILITY_NULL, 1);
    *addr = 0;
    return sys_memory_allocate(size, SYS_MEMORY_PAGE_SIZE_1M, addr);
}

/*
 * Free a buffer returned by gcm_host_buffer_alloc().  Passing 0 is a
 * no-op returning CELL_OK.
 */
static inline int gcm_host_buffer_free(sys_addr_t addr)
{
    if (addr == 0)
        return CELL_OK;
    return sys_memory_free(addr);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3TC_SAMPLES_GCM_COMMON_HOST_BUFFER_H__ */
