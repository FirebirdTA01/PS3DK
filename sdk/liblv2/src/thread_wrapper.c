/*
 * liblv2 — sysThreadCreate wrapper (clean-room).
 *
 * Behavioral contract (re-derived, not copied):
 *   The SPRX-exported primitive sysThreadCreateEx takes the thread
 *   entry as a compact-OPD effective address (opd32 *), because the
 *   kernel/loader resolves the descriptor itself.  A C function
 *   pointer under our compact-OPD ABI (docs/abi/cellos-lv2-abi-spec.md
 *   §2) already IS that 32-bit descriptor EA, so the public
 *   sysThreadCreate just narrows the C function pointer to the EA via
 *   __get_opd32 and forwards every other argument verbatim.
 *
 *   sysThreadCreateEx is provided by the nidgen-emitted liblv2_stub.a
 *   (sysPrxForUser NID 0x24a1ea07 / sys_ppu_thread_create), multilib
 *   ILP32 + LP64.  This wrapper carries no syscall logic of its own.
 */

/* Include the SDK headers that transitively pull the legacy
 * <ppu-asm.h> FIRST, then our <sys/ppu-asm.h> LAST.  Our header is
 * #ifndef-guarded per symbol, so when the legacy macros/inlines are
 * already in scope it defers entirely and no redefinition occurs.
 * Reversing this order makes the unguarded legacy header redefine our
 * symbols (hard error on the bswap inlines). */
#include <sys/thread.h>
#include <sys/ppu-asm.h>

/* SPRX import (resolved from liblv2_stub.a at link time). */
extern s32 sysThreadCreateEx(sys_ppu_thread_t *threadid,
                             opd32 *opdentry,
                             void *arg,
                             s32 priority,
                             u64 stacksize,
                             u64 flags,
                             char *threadname);

s32 sysThreadCreate(sys_ppu_thread_t *threadid,
                    void (*entry)(void *),
                    void *arg,
                    s32 priority,
                    u64 stacksize,
                    u64 flags,
                    char *threadname)
{
    /* Compact-OPD ABI: the function pointer's value is the 32-bit
     * descriptor EA the SPRX primitive expects.  __get_opd32 is the
     * NULL-preserving identity that yields that EA. */
    opd32 *entry_ea = (opd32 *)(uintptr_t)__get_opd32(entry);

    return sysThreadCreateEx(threadid, entry_ea, arg,
                             priority, stacksize, flags, threadname);
}
