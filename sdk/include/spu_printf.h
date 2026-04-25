/* spu_printf.h - printf forwarding from SPU programs to PPU-side stdio.
 *
 * Two PPU-side entry points:
 *   spu_printf_initialize(priority, handler)
 *     Spawns a PPU helper thread (priority `priority`) that listens for
 *     printf interrupts from SPU threads.  `handler` is an optional
 *     callback (NULL to use libc's built-in handler).
 *   spu_printf_finalize()
 *     Tears down the helper thread.
 *
 * Both resolve to NIDs in libc_stub.a.  SPU-side code uses raw_spu_printf
 * (separate SPU-library API, not declared here).
 */
#ifndef __PS3DK_SPU_PRINTF_H__
#define __PS3DK_SPU_PRINTF_H__

#include <lv2/spu.h>     /* PSL1GHT sysSpuPrintf* PRX-export wrappers */

#ifdef __cplusplus
extern "C" {
#endif

/* spu_printf_*: reference-SDK names over PSL1GHT's working sysSpuPrintf*.
 *
 * Why we don't go through cellSysutil_libc.spu_printf_* (NIDs from
 * libc_stub) directly: RPCS3's HLE for those entries doesn't actually
 * wire the SPU thread group's printf event port (port 1) to a
 * PPU-side event queue.  SPU-side spu_printf() then calls
 * sys_spu_thread_send_event(spup=1, ...) and gets back CELL_ENOTCONN
 * because nothing is listening.
 *
 * PSL1GHT's sysSpuPrintf* in liblv2.sprx implements the real
 * connect-and-handler pattern (event queue + ppu thread + spuThreadGroupConnectEvent).
 * Forwarding our reference-SDK names through them gives us the same
 * source-compat surface plus a working printf path under RPCS3 / real
 * hardware. */

static inline int spu_printf_initialize(int priority, void (*handler)(const char *))
{
    /* PSL1GHT entry-point type is `void (*)(void *)` — a thread entry,
     * not a per-line-handler.  When `handler` is NULL we hand NULL
     * through and PSL1GHT installs its built-in printf-server thread.
     * Reference-SDK callers who pass a per-line handler are not
     * supported here; treat them as opting into PSL1GHT's default. */
    (void)handler;
    return (int)sysSpuPrintfInitialize(priority, NULL);
}

static inline int spu_printf_finalize(void)
{
    return (int)sysSpuPrintfFinalize();
}

static inline int spu_printf_attach_group(unsigned int group)
{
    return (int)sysSpuPrintfAttachGroup((sys_spu_group_t)group);
}

static inline int spu_printf_attach_thread(unsigned int thread)
{
    return (int)sysSpuPrintfAttachThread((sys_spu_thread_t)thread);
}

static inline int spu_printf_detach_group(unsigned int group)
{
    return (int)sysSpuPrintfDetachGroup((sys_spu_group_t)group);
}

static inline int spu_printf_detach_thread(unsigned int thread)
{
    return (int)sysSpuPrintfDetachThread((sys_spu_thread_t)thread);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SPU_PRINTF_H__ */
