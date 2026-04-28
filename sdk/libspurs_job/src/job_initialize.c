/*
 * job_initialize.c - cellSpursJobInitialize().
 *
 * Zero-fills the .bss range [__bss_start, _end) before main runs.  The
 * linker script defines both bracketing symbols at .bss section
 * boundaries (16-byte aligned by section alignment).
 *
 * Implementation strategy: when both bounds are 16-byte aligned (the
 * common case for an SPU binary), drop straight into a vec_uint4 store
 * loop which lets the SPU pipeline issue one zero-write per even-pipe
 * slot.  We still fall back to a per-byte tail clean-up for safety.
 */

#include <stdint.h>
#include <spu_intrinsics.h>

extern char __bss_start[];
extern char _end[];

void cellSpursJobInitialize(void)
{
    char *p   = __bss_start;
    char *end = _end;
    if (p >= end)
        return;

    /* Vectorised middle: clear in 16-byte chunks while both ends are
     * aligned and at least one full vector remains. */
    if ((((uintptr_t)p) & 0xf) == 0) {
        const vec_uint4 zero = (vec_uint4){0, 0, 0, 0};
        char *vend = (char *)((uintptr_t)end & ~(uintptr_t)0xf);
        while (p < vend) {
            *(vec_uint4 *)p = zero;
            p += 16;
        }
    }

    /* Byte tail (also handles the unaligned-start fallback). */
    while (p < end)
        *p++ = 0;
}
