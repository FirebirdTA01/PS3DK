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

#ifdef __cplusplus
extern "C" {
#endif

extern int spu_printf_initialize(int priority, void (*handler)(const char *));
extern int spu_printf_finalize(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SPU_PRINTF_H__ */
