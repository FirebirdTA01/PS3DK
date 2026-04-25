/* libc_stub_extras: spu_printf_* — working PPU-side replacement for the
 * libc.sprx wrappers that ship in the same library.
 *
 * Why this file exists.  SPU code emits its printfs through
 * `_spu_call_event_va_arg(EVENT_PRINTF_PORT << EVENT_PORT_SHIFT, fmt, ...)`,
 * which under the hood does `sys_spu_thread_send_event(spup=1, ...)`.
 * For that send to deliver, port 1 of every thread in the group must
 * be connected to a PPU-side event queue via
 * `sys_spu_thread_group_connect_event_all_threads(group, queue, mask, &spup)`.
 *
 * Both PSL1GHT's wrappers and (as observed in RPCS3 traces) the
 * libc.sprx HLE attempt the connection through the wrong syscall —
 * `sys_spu_thread_group_connect_event(group, queue, et=USER)` — which
 * connects an event TYPE rather than a per-thread port number.  The
 * SPU's send_event then fails with CELL_ENOTCONN and the printf is
 * silently dropped.
 *
 * This translation unit defines the user-facing
 * spu_printf_{initialize,finalize,attach_group,attach_thread,
 * detach_group,detach_thread} as real PPU functions doing the right
 * thing, and gets ar-appended to libc_stub.a after the FNID stub
 * generation step (see scripts/build-cell-stub-archives.sh).  The
 * yaml entries for these six exports have been elided so there is
 * no competing FNID stub to collide with.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#include <ppu-types.h>
#include <sys/spu.h>                  /* sysSpuThread* + types */
#include <sys/event_queue.h>          /* sys_event_queue_attr_t etc. */
#include <sys/thread.h>               /* sysThreadCreate / Join / Exit */
#include <sys/spu_thread_printf.h>    /* spu_thread_printf — formats LS args */

/* ---- internal state (single global server) ----------------------- */
static int               s_initialized;
static sys_event_queue_t s_event_queue;
static sys_event_port_t  s_terminating_port;
static sys_ppu_thread_t  s_handler_thread;

/* Maximum number of in-flight printfs the queue can hold.  127 matches
 * the pre-existing PSL1GHT default and the kernel's queue cap. */
#define SPU_PRINTF_QUEUE_DEPTH        127

/* "Magic" token used as the event source for the terminate-thread
 * port.  Any 32-bit value the kernel won't otherwise generate works;
 * picking a memorable one keeps log diagnostics readable. */
#define SPU_PRINTF_TERMINATE_TAG      0x53505550 /* 'SPUP' */

/* Per-thread connection state.  Up to 8 thread groups (or threads)
 * concurrently attached.  The event-port 1 mask is fixed across all
 * connections so the simple table is enough. */
#define SPU_PRINTF_PORT               1u
/* sys_spu_thread_group_connect_event_all_threads `req` mask: the
 * kernel matches the bit at position (63 - port_number) when the
 * caller asks for a specific port.  Port 1 -> bit 62 -> 0x4000... */
#define SPU_PRINTF_PORT_REQ           ((u64)1 << (63 - SPU_PRINTF_PORT))

/* ---- server thread ----------------------------------------------- *
 *
 * Loops forever receiving events from s_event_queue.  When an SPU
 * printf event arrives, sys_event_t.data_1 is the originating SPU
 * thread id and data_3 is the LS address of the saved-args block.
 * spu_thread_printf reads the format string + args from SPU LS and
 * delegates to vprintf-equivalent.  A non-zero return must be
 * mailbox'd back to the SPU so its `_spu_call_event_va_arg` can
 * unblock; PSL1GHT's helper does that for us via sysSpuThreadWriteMb.
 *
 * The terminate signal arrives as event.source ==
 * SPU_PRINTF_TERMINATE_TAG, sent by spu_printf_finalize via
 * s_terminating_port.
 */
static void spu_printf_server_entry(void *arg)
{
    (void)arg;

    for (;;) {
        sys_event_t event;
        s32 rc = sysEventQueueReceive(s_event_queue, &event, 0);
        if (rc) {
            sysThreadExit(rc);
        }

        if (event.source == SPU_PRINTF_TERMINATE_TAG) {
            sysThreadExit(0);
        }

        sys_spu_thread_t thread_id = (sys_spu_thread_t)event.data_1;
        u32              arg_addr  = (u32)event.data_3;

        s32 sret = spu_thread_printf(thread_id, arg_addr);
        rc = sysSpuThreadWriteMb(thread_id, (u32)sret);
        if (rc) {
            sysThreadExit(rc);
        }
    }
}

/* ---- public surface --------------------------------------------- */

int spu_printf_initialize(int priority, void *unused)
{
    (void)unused;

    if (s_initialized) {
        return 0;
    }

    sys_event_queue_attr_t attr;
    attr.attr_protocol = SYS_EVENT_QUEUE_PRIO;
    attr.type          = SYS_EVENT_QUEUE_PPU;
    attr.name[0]       = '\0';

    s32 rc = sysEventQueueCreate(&s_event_queue, &attr,
                                 SYS_EVENT_QUEUE_KEY_LOCAL,
                                 SPU_PRINTF_QUEUE_DEPTH);
    if (rc) {
        return rc;
    }

    rc = sysThreadCreate(&s_handler_thread, spu_printf_server_entry, NULL,
                         priority, 4096, THREAD_JOINABLE,
                         "spu_printf_handler");
    if (rc) {
        sysEventQueueDestroy(s_event_queue, 0);
        return rc;
    }

    rc = sysEventPortCreate(&s_terminating_port, SYS_EVENT_PORT_LOCAL,
                            SPU_PRINTF_TERMINATE_TAG);
    if (rc) {
        return rc;
    }

    rc = sysEventPortConnectLocal(s_terminating_port, s_event_queue);
    if (rc) {
        return rc;
    }

    s_initialized = 1;
    return 0;
}

int spu_printf_finalize(void)
{
    if (!s_initialized) {
        return 0;
    }

    s32 rc = sysEventPortSend(s_terminating_port, 0, 0, 0);
    if (rc) return rc;

    u64 exit_code = 0;
    rc = sysThreadJoin(s_handler_thread, &exit_code);
    if (rc) return rc;

    rc = sysEventPortDisconnect(s_terminating_port);
    if (rc) return rc;

    rc = sysEventPortDestroy(s_terminating_port);
    if (rc) return rc;

    rc = sysEventQueueDestroy(s_event_queue, 0);
    if (rc) return rc;

    s_initialized = 0;
    return 0;
}

int spu_printf_attach_group(unsigned int group)
{
    if (!s_initialized) return -1;

    u8 spup = 0;
    /* Request port 1 specifically.  The kernel writes back the actual
     * port number assigned in `spup`; for a single-bit `req` mask the
     * assignment must match what was asked for, so the SPU side's
     * hard-coded EVENT_PRINTF_PORT (=1) lines up. */
    return (int)sysSpuThreadGroupConnectEventAllThreads(
        (sys_spu_group_t)group, s_event_queue,
        SPU_PRINTF_PORT_REQ, &spup);
}

int spu_printf_detach_group(unsigned int group)
{
    if (!s_initialized) return -1;
    return (int)sysSpuThreadGroupDisonnectEventAllThreads(
        (sys_spu_group_t)group, SPU_PRINTF_PORT);
}

int spu_printf_attach_thread(unsigned int thread)
{
    if (!s_initialized) return -1;
    /* Connect the SPU thread's user event port to our queue.  The
     * single-thread variant uses sys_spu_thread_connect_event with
     * the SPU_THREAD_EVENT_USER class and the same port number. */
    return (int)sysSpuThreadConnectEvent((sys_spu_thread_t)thread,
                                         s_event_queue,
                                         SPU_THREAD_EVENT_USER,
                                         SPU_PRINTF_PORT);
}

int spu_printf_detach_thread(unsigned int thread)
{
    if (!s_initialized) return -1;
    return (int)sysSpuThreadDisconnectEvent((sys_spu_thread_t)thread,
                                            SPU_THREAD_EVENT_USER,
                                            SPU_PRINTF_PORT);
}
