/*
 * liblv2 — SPU printf server (independent).
 *
 * Behavioral contract (re-derived from the documented protocol, not
 * copied):
 *
 *   The SPU "printf via syscall" mechanism works by the SPU raising a
 *   user event on a known SPU event port; a PPU-side handler thread
 *   drains an event queue, and for each event:
 *     - event.source == terminate-sentinel  -> the handler exits
 *     - otherwise event.data_1 = the originating SPU thread id and
 *       event.data_3 = the LS argument-block address; the handler
 *       formats the message with spu_thread_printf(id, arg_addr),
 *       then writes the produced byte count back to the SPU via the
 *       inbound mailbox so the SPU can continue.
 *
 *   Initialize:
 *     - create a prio event queue (local key)
 *     - spawn the handler thread (joinable)
 *     - create + connect a local event port used only to wake the
 *       handler for shutdown
 *   Finalize:
 *     - send the terminate event, join the handler, then tear the
 *       port and queue down in reverse order
 *   Attach/Detach (thread / group):
 *     - connect/disconnect the queue to the SPU thread (or group) on
 *       the USER event class with the printf SPU port number
 *
 *   Every primitive used here is an inline LV2 syscall wrapper or an
 *   SPRX import from elsewhere in the SDK; this file is pure glue.
 */

/* SDK headers first (they pull the legacy <ppu-asm.h>), our guarded
 * <sys/ppu-asm.h> last — see thread_wrapper.c for the rationale. */
#include <stddef.h>
#include <sys/spu.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sys/spu_thread_printf.h>
#include <sys/ppu-asm.h>

/* SPU event port number reserved for the printf channel. */
#define LV2_SPU_PRINTF_PORT      1
/* Event-queue depth: deep enough that bursts of SPU prints from
 * several threads don't drop, shallow enough to stay cheap. */
#define LV2_SPU_PRINTF_QDEPTH    127
/* Sentinel event.source used by the shutdown port so the handler can
 * distinguish "terminate" from a real printf event. */
#define LV2_SPU_PRINTF_TERM_NAME 0xFEE1DEADU

static u32              g_initialized;
static sys_event_queue_t g_queue;
static sys_ppu_thread_t  g_handler;
static sys_event_port_t  g_term_port;

/* Default handler-thread body: drains the queue forever, servicing
 * printf events, until the shutdown sentinel arrives. */
static void lv2_spu_printf_handler(void *arg)
{
    (void)arg;

    for (;;) {
        sys_event_t       ev;
        sys_spu_thread_t  spu_tid;
        s32               produced;
        s32               r;

        if (sysEventQueueReceive(g_queue, &ev, 0) != 0)
            continue;

        if (ev.source == LV2_SPU_PRINTF_TERM_NAME)
            sysThreadExit(0);

        spu_tid  = (sys_spu_thread_t)ev.data_1;
        produced = spu_thread_printf(spu_tid, (u32)ev.data_3);

        r = sysSpuThreadWriteMb(spu_tid, (u32)produced);
        if (r != 0)
            sysThreadExit((u64)-1);
    }
}

s32 sysSpuPrintfInitialize(int prio, void (*entry)(void *))
{
    sys_event_queue_attr_t qattr = {
        SYS_EVENT_QUEUE_PRIO, SYS_EVENT_QUEUE_PPU, ""
    };
    void (*body)(void *) = entry ? entry : lv2_spu_printf_handler;
    s32 r;

    if (g_initialized)
        return 0;

    r = sysEventQueueCreate(&g_queue, &qattr,
                            SYS_EVENT_QUEUE_KEY_LOCAL,
                            LV2_SPU_PRINTF_QDEPTH);
    if (r != 0)
        return r;

    r = sysThreadCreate(&g_handler, body, &g_queue,
                        prio, 4096, THREAD_JOINABLE,
                        "spu_printf_handler");
    if (r != 0)
        return r;

    r = sysEventPortCreate(&g_term_port, SYS_EVENT_PORT_LOCAL,
                           LV2_SPU_PRINTF_TERM_NAME);
    if (r != 0)
        return r;

    r = sysEventPortConnectLocal(g_term_port, g_queue);
    if (r != 0)
        return r;

    g_initialized = 1;
    return 0;
}

s32 sysSpuPrintfAttachThread(sys_spu_thread_t thread)
{
    if (!g_initialized)
        return -1;
    return sysSpuThreadConnectEvent(thread, g_queue,
                                    SPU_THREAD_EVENT_USER,
                                    LV2_SPU_PRINTF_PORT);
}

s32 sysSpuPrintfDetachThread(sys_spu_thread_t thread)
{
    if (!g_initialized)
        return -1;
    return sysSpuThreadDisconnectEvent(thread,
                                       SPU_THREAD_EVENT_USER,
                                       LV2_SPU_PRINTF_PORT);
}

s32 sysSpuPrintfAttachGroup(sys_spu_group_t group)
{
    if (!g_initialized)
        return -1;
    return sysSpuThreadGroupConnectEvent(group, g_queue,
                                         SPU_THREAD_EVENT_USER);
}

s32 sysSpuPrintfDetachGroup(sys_spu_group_t group)
{
    if (!g_initialized)
        return -1;
    return sysSpuThreadGroupDisconnectEvent(group,
                                            SPU_THREAD_EVENT_USER);
}

s32 sysSpuPrintfFinalize(void)
{
    u64 exit_code;
    s32 r;

    if (!g_initialized)
        return 0;

    /* Wake the handler with the shutdown sentinel and wait it out. */
    r = sysEventPortSend(g_term_port, 0, 0, 0);
    if (r != 0)
        return r;
    r = sysThreadJoin(g_handler, &exit_code);
    if (r != 0)
        return r;

    /* Tear down in reverse order of construction. */
    r = sysEventPortDisconnect(g_term_port);
    if (r != 0)
        return r;
    r = sysEventPortDestroy(g_term_port);
    if (r != 0)
        return r;
    r = sysEventQueueDestroy(g_queue, 0);
    if (r != 0)
        return r;

    g_initialized = 0;
    return 0;
}
