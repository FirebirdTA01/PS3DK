/*
 * samples/gcm/common/gcm_host_buffer.h
 *
 * Helper for the GCM/RSX host-buffer allocation.  The GCM samples used
 * to take the host buffer they pass to cellGcmInit() / rsxInit() from
 * the malloc heap:
 *
 *     host = memalign(1024*1024, HOST_SIZE);
 *     ... cellGcmInit(cmdsize, HOST_SIZE, host); ...
 *
 * The PS3 user sbrk arena is a fixed 64 MiB pool shared with everything
 * else in the process, so a 16 or 32 MiB host buffer taken from it
 * leaves an application copying the pattern that much less heap.  (The
 * EMP engine lost 32 MiB this way.)
 *
 * Instead, take the buffer straight from LV2: sys_memory_allocate() with
 * SYS_MEMORY_PAGE_SIZE_1M pages backs it without touching the sbrk
 * arena, and 1 MiB pages make the start 1 MiB aligned, as the RSX IO
 * mapping requires.  The size must be a non-zero multiple of 1 MiB; the
 * helper refuses anything else instead of letting the IO mapping fail
 * later.
 *
 * Usage:
 *
 *     #include "../../common/gcm_host_buffer.h"
 *
 *     sys_addr_t host = 0;
 *     if (gcm_host_buffer_alloc(HOST_SIZE, &host) != CELL_OK) { ...fail... }
 *     ... cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host); ...
 *
 * Lifetime: once cellGcmInit() / rsxInit() has been called with the
 * buffer, the RSX maps it for IO and may keep fetching commands from it,
 * so it must stay allocated for the rest of the program; process exit
 * reclaims it.  Call gcm_host_buffer_free() only for a buffer GCM never
 * saw, or after the RSX is idle and the buffer's IO mapping is gone.
 *
 * Both functions are static inline, so the header needs no library and
 * builds for ILP32 and LP64 alike.
 */

#ifndef PS3TC_SAMPLES_GCM_COMMON_HOST_BUFFER_H
#define PS3TC_SAMPLES_GCM_COMMON_HOST_BUFFER_H

#include <stdint.h>
#include <ppu-types.h>
#include <sys/memory.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCM_HOST_BUFFER_PAGE (1024u * 1024u)

/* Returned for a NULL out-pointer or a size that is zero or not a
 * multiple of 1 MiB: lv2's invalid-argument status, 0x80010002. */
#define GCM_HOST_BUFFER_EINVAL CELL_ERROR_CAST(0x80010002)

/*
 * Allocate `size` bytes of 1 MiB LV2 pages.  On success returns CELL_OK
 * and stores the 1 MiB-aligned start address in *addr.  On failure
 * returns GCM_HOST_BUFFER_EINVAL or the error from sys_memory_allocate()
 * and leaves *addr (when addr is not NULL) at 0.
 */
static inline int gcm_host_buffer_alloc(uint32_t size, sys_addr_t *addr)
{
    int rc;

    if (addr == NULL)
        return GCM_HOST_BUFFER_EINVAL;
    *addr = 0;
    if (size == 0 || (size % GCM_HOST_BUFFER_PAGE) != 0)
        return GCM_HOST_BUFFER_EINVAL;
    rc = sys_memory_allocate(size, SYS_MEMORY_PAGE_SIZE_1M, addr);
    if (rc != CELL_OK)
        *addr = 0;
    return rc;
}

/*
 * Free a buffer from gcm_host_buffer_alloc() (see the lifetime note
 * above).  Freeing 0 is a no-op returning CELL_OK.
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

#endif /* PS3TC_SAMPLES_GCM_COMMON_HOST_BUFFER_H */
