/* spu_printf.h - PPU-side surface for SPU printf forwarding.
 *
 * Six entry points, all resolved by linker against libc_stub.a:
 *
 *   spu_printf_initialize(priority, handler)
 *     Set up the PPU-side event-queue + server thread that consumes
 *     printf events from SPU code.  `priority` is the priority of
 *     the server thread; `handler` is reserved (pass NULL).  Must be
 *     called once before any attach call.
 *
 *   spu_printf_finalize()
 *     Send the terminate signal, join the server thread, dispose of
 *     the event queue.
 *
 *   spu_printf_attach_group(group) / detach_group(group)
 *     Connect / disconnect every thread in the SPU thread group's
 *     printf event-port (port 1) to the PPU server's queue.
 *
 *   spu_printf_attach_thread(thread) / detach_thread(thread)
 *     Same but for a single SPU thread.
 *
 * The reference SDK ships these in libc.sprx; the SPRX-side
 * implementation (and RPCS3's HLE for it) does not connect port 1
 * via sys_spu_thread_group_connect_event_all_threads, so SPU
 * spu_printf() calls deliver CELL_ENOTCONN and the message is
 * silently dropped.  Our libc_stub.a ships a working real-PPU
 * replacement under the same names — see
 * sdk/libc_stub_extras/src/spu_printf.c.
 *
 * SPU-side code uses spu_printf() from <spu_printf.h> (separate
 * SPU-library API, declared elsewhere).
 */
#ifndef __PS3DK_SPU_PRINTF_H__
#define __PS3DK_SPU_PRINTF_H__

#ifdef __cplusplus
extern "C" {
#endif

extern int spu_printf_initialize(int priority, void *unused);
extern int spu_printf_finalize(void);

extern int spu_printf_attach_group(unsigned int group);
extern int spu_printf_attach_thread(unsigned int thread);
extern int spu_printf_detach_group(unsigned int group);
extern int spu_printf_detach_thread(unsigned int thread);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SPU_PRINTF_H__ */
