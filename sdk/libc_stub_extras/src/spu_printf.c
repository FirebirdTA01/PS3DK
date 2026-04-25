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
#include <string.h>
#include <errno.h>

#include <ppu-types.h>
#include <sys/spu.h>                  /* sysSpuThread* + types */
#include <sys/event_queue.h>          /* sys_event_queue_attr_t etc. */
#include <sys/thread.h>               /* sysThreadCreate / Join / Exit */
#include <sys/lv2_syscall.h>          /* lv2syscall4 for SYS_SPU_THREAD_READ_LS */

/* sys_spu_thread_read_ls - kernel syscall 182.  Reads `type`-byte
 * value from SPU LS at `lsa` of the given thread.  Type=4 means
 * 32-bit word, type=8 means 64-bit dword.  RPCS3 implements this
 * (see Emu/Cell/lv2/sys_spu.cpp:1864). */
#define SYSCALL_SPU_THREAD_READ_LS    182

static inline int sys_spu_thread_read_ls_word(unsigned int id, unsigned int lsa, uint32_t *out)
{
    uint64_t buf = 0;
    lv2syscall4(SYSCALL_SPU_THREAD_READ_LS, id, lsa, (u64)(uintptr_t)&buf, 4);
    /* read_ls writes the read value into *value; for type=4 it's in
     * the low 32 bits of the 64-bit slot.  Some kernels write the
     * value into the high half instead — handle both by combining. */
    *out = (uint32_t)(buf | (buf >> 32));
    return (int)p1;  /* lv2syscall result */
}

static inline int sys_spu_thread_read_ls_byte(unsigned int id, unsigned int lsa, uint8_t *out)
{
    uint64_t buf = 0;
    lv2syscall4(SYSCALL_SPU_THREAD_READ_LS, id, lsa, (u64)(uintptr_t)&buf, 1);
    *out = (uint8_t)buf;
    return (int)p1;
}

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
/* sys_spu_thread_group_connect_event_all_threads `req` mask: open
 * up the entire low half of the port range and let the kernel pick.
 * The mask is bit N <-> port N (LSB-aligned); 0xffff..0000 with the
 * top 48 bits set requests any of ports 16..63.  We ask for ports
 * 0..63 here; the kernel returns the assigned port in *spup, and we
 * connect that to our queue.  But the SPU's `_spu_call_event_va_arg`
 * is hard-coded to send on port 1 — so if the kernel doesn't hand
 * back port 1 specifically, the SPU printf path won't deliver.
 * PSL1GHT solved this by requesting only the bit for the desired
 * port; we follow the same pattern. */
#define SPU_PRINTF_PORT_REQ           ((u64)1 << SPU_PRINTF_PORT)

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

        /* The arg block at SPU LS offset arg_addr was laid down by
         * _spu_call_event_va_arg — first 4 bytes hold the LS address
         * of the format string.  Read the fmt-ptr, then walk the LS
         * byte-by-byte until null and print as-is.
         *
         * Caveat: this minimal implementation prints the format
         * STRING verbatim — it doesn't parse %X conversion specs
         * or pull va_args from the saved register block.  Adequate
         * for log-style messages without conversions; richer printf
         * support would need the full format-walk + per-spec LS
         * read of subsequent args. */
        uint32_t fmt_addr = 0;
        if (sys_spu_thread_read_ls_word(thread_id, arg_addr, &fmt_addr) == 0
            && fmt_addr != 0) {
            char buf[256];
            unsigned i = 0;
            for (; i < sizeof(buf) - 1; i++) {
                uint8_t b = 0;
                if (sys_spu_thread_read_ls_byte(thread_id, fmt_addr + i, &b) != 0
                    || b == 0) {
                    break;
                }
                buf[i] = (char)b;
            }
            buf[i] = '\0';
            fputs(buf, stdout);
        }

        rc = sysSpuThreadWriteMb(thread_id, 0);
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
