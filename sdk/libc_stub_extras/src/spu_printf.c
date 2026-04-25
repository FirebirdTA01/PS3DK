/* libc_stub_extras: spu_printf_* — working PPU-side replacement for the
 * libc.sprx wrappers that ship in the same library.
 *
 * Why this file exists (part 1: connect-event).  SPU code emits its
 * printfs through `_spu_call_event_va_arg(EVENT_PRINTF_PORT <<
 * EVENT_PORT_SHIFT, fmt, ...)`, which under the hood does
 * `sys_spu_thread_send_event(spup=1, ...)`.  For that send to deliver,
 * port 1 of every thread in the group must be connected to a PPU-side
 * event queue via `sys_spu_thread_group_connect_event_all_threads(group,
 * queue, mask, &spup)` with `mask = (1ULL << 1)` for port 1
 * specifically (the kernel iterates ports 0..63 in ascending order
 * and connects the first port whose bit is set in `mask`; LSB-aligned).
 *
 * Both PSL1GHT's wrappers and (as observed in RPCS3 traces) the
 * libc.sprx HLE attempt the connection through the wrong syscall —
 * `sys_spu_thread_group_connect_event(group, queue, et=USER)` — which
 * connects an event TYPE rather than a per-thread port number.  The
 * SPU's send_event then fails with CELL_ENOTCONN and the printf is
 * silently dropped.
 *
 * Why this file exists (part 2: format-string read).  Once the event
 * is delivered to our queue, we still have to walk the SPU thread's
 * Local Storage at the address the event carried to extract the
 * format string + args.  The reference SDK ships a helper for this:
 * `spu_thread_printf(thread_id, arg_addr)` (in libc.sprx, NID
 * 0xb1f4779d).  RPCS3's HLE for that NID is **unimplemented at the
 * time of writing** — calling it logs `PPU: Unregistered function
 * called` and returns nothing useful.  On real PS3 hardware (and any
 * future RPCS3 build that implements the NID) it formats the message
 * correctly with full %d/%s/%lld variadic support.
 *
 * The `SPU_PRINTF_USE_SPRX_FORMATTER` build-time switch picks between
 * the two paths:
 *
 *   0 (default, RPCS3-friendly): call sys_spu_thread_read_ls (kernel
 *     syscall 182, RPCS3 *does* implement this) directly to read the
 *     fmt-string pointer + bytes from SPU LS, then fputs to PPU
 *     stdout.  Limitation: prints the format string verbatim — no
 *     %d / %s / %lld conversion.  Adequate for log-style strings
 *     without specifiers.
 *
 *   1 (real-hardware-canonical): call `spu_thread_printf` directly.
 *     Works on real HW and any RPCS3 with HLE for NID 0xb1f4779d;
 *     produces nothing on current RPCS3.  Drop-in if/when that HLE
 *     gap closes.
 *
 * This translation unit defines the user-facing
 * spu_printf_{initialize,finalize,attach_group,attach_thread,
 * detach_group,detach_thread} as real PPU functions doing the right
 * thing, and gets ar-appended to libc_stub.a after the FNID stub
 * generation step (see scripts/build-cell-stub-archives.sh).  The
 * yaml entries for those six exports have been elided so there is
 * no competing FNID stub to collide with.  The seventh entry
 * (spu_thread_printf, NID 0xb1f4779d) is left in the FNID stub set
 * so users who want the SPRX path on real HW can call it directly.
 */

/* Default: use the RPCS3-friendly direct-LS-read path.  Set to 1 in
 * a downstream Makefile or `-DSPU_PRINTF_USE_SPRX_FORMATTER=1` to
 * route through libc.sprx's spu_thread_printf instead. */
#ifndef SPU_PRINTF_USE_SPRX_FORMATTER
#define SPU_PRINTF_USE_SPRX_FORMATTER 0
#endif

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

#if SPU_PRINTF_USE_SPRX_FORMATTER
#include <sys/spu_thread_printf.h>    /* spu_thread_printf — libc.sprx helper */
#endif

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

static inline int sys_spu_thread_read_ls_dword(unsigned int id, unsigned int lsa, uint64_t *out)
{
    uint64_t buf = 0;
    lv2syscall4(SYSCALL_SPU_THREAD_READ_LS, id, lsa, (u64)(uintptr_t)&buf, 8);
    *out = buf;
    return (int)p1;
}

/* Read up to `cap-1` bytes of a null-terminated string from SPU LS
 * starting at `lsa` into `buf`.  Always null-terminates.  Used for
 * both the format string itself and any `%s` arg pointers. */
static unsigned read_spu_ls_string(unsigned int thread_id, uint32_t lsa,
                                   char *buf, size_t cap)
{
    unsigned i = 0;
    if (cap == 0) return 0;
    for (; i < cap - 1; i++) {
        uint8_t b = 0;
        if (sys_spu_thread_read_ls_byte(thread_id, lsa + i, &b) != 0 || b == 0) {
            break;
        }
        buf[i] = (char)b;
    }
    buf[i] = '\0';
    return i;
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

/* ---- format walker (Path B's printf-spec handler) --------------- *
 *
 * The arg block at SPU LS offset `arg_addr` was laid down by
 * `_spu_call_event_va_arg`.  Layout (16 quadwords, 256 bytes total):
 *
 *   arg_addr +   0..15: caller's $4 — fmt pointer in bytes 0..3
 *                      (preferred-slot for 32-bit values on SPU)
 *   arg_addr +  16..31: $5 — first vararg slot
 *   arg_addr +  32..47: $6 — second vararg slot
 *   ...
 *   arg_addr + 240..255: $19 — fifteenth vararg slot
 *
 * Within a slot, the SPU SysV ABI puts:
 *   32-bit int / pointer / float (varargs DON'T default-promote
 *     float -> double on SPU like they do on PPC, but the compiler
 *     emits doubles to %f anyway when fed a float — meh, depends on
 *     caller; treat %f as 8-byte to match standard C printf
 *     conventions): bytes 0..3 of slot
 *   64-bit long long / double: bytes 0..7 of slot
 *
 * The walker copies non-format text verbatim and for each %-spec:
 *   1) collects the spec ('%' through conversion char) into a tiny
 *      buffer
 *   2) sniffs the length-modifier (none, l, ll, h, hh, j, z, t)
 *   3) reads the appropriate-sized value from the next slot via
 *      sys_spu_thread_read_ls (8-byte read for %lld/%llu/%llx/%f
 *      family, 4-byte read otherwise)
 *   4) hands it to snprintf with the collected spec
 *   5) advances the slot index by 1 (each vararg consumes one
 *      16-byte slot regardless of size)
 *
 * %s gets a second LS round-trip: the slot holds a 32-bit pointer
 * into SPU LS where the actual string lives.  Read up to 256 bytes
 * of it before snprintf'ing.
 *
 * Output is collected into `out_buf` and written to PPU stdout via
 * a single fputs at the end so partial writes can't interleave with
 * other threads' output. */

#define SPU_PRINTF_OUT_BUFCAP   1024
#define SPU_PRINTF_SPEC_BUFCAP  64
#define SPU_PRINTF_S_BUFCAP     256
#define SPU_PRINTF_MAX_VARARGS  15  /* slots $5..$19 */

static int is_spu_printf_conversion(char c)
{
    switch (c) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
    case 'c': case 's': case 'p':
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
    case 'a': case 'A':
    case 'n':
    case '%':
        return 1;
    default:
        return 0;
    }
}

static void append(char *buf, size_t cap, size_t *len, const char *src)
{
    size_t avail = cap - *len - 1;
    size_t n = strlen(src);
    if (n > avail) n = avail;
    memcpy(buf + *len, src, n);
    *len += n;
    buf[*len] = '\0';
}

/* Format-aware version of the SPU printf path.  Returns the number
 * of bytes written to `out_buf` (excluding trailing null), or -1 on
 * read-LS failure.  Does NOT call fputs; the caller is responsible
 * for emitting the buffer.  Stays under SPU_PRINTF_OUT_BUFCAP-1
 * bytes and always null-terminates. */
static int format_spu_printf(unsigned int thread_id, uint32_t arg_addr,
                             char *out_buf)
{
    out_buf[0] = '\0';
    size_t out_len = 0;

    /* fmt pointer is in bytes 0..3 of the first slot (caller's $4). */
    uint32_t fmt_addr = 0;
    if (sys_spu_thread_read_ls_word(thread_id, arg_addr, &fmt_addr) != 0
        || fmt_addr == 0) {
        return -1;
    }

    /* Read the entire format string into a local buffer up front so
     * we don't pay an LS read per character of literal text. */
    char fmt[SPU_PRINTF_OUT_BUFCAP];
    read_spu_ls_string(thread_id, fmt_addr, fmt, sizeof(fmt));

    int slot = 1;  /* next vararg lives at slot 1 (offset 16); slot 0 was fmt */

    for (const char *p = fmt; *p; ) {
        if (*p != '%') {
            char one[2] = { *p++, '\0' };
            append(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, one);
            continue;
        }
        /* '%' — collect spec up to and including the conversion char. */
        char spec[SPU_PRINTF_SPEC_BUFCAP];
        size_t sp = 0;
        spec[sp++] = '%';
        p++;
        while (*p && sp < sizeof(spec) - 1 && !is_spu_printf_conversion(*p)) {
            spec[sp++] = *p++;
        }
        if (!*p) break;  /* truncated spec */
        char conv = *p++;
        spec[sp++] = conv;
        spec[sp]   = '\0';

        if (conv == '%') {
            append(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, "%");
            continue;
        }
        if (slot > SPU_PRINTF_MAX_VARARGS) {
            /* Out of saved-register slots; print spec verbatim and stop. */
            append(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, spec);
            continue;
        }

        /* Detect length modifier.  We only need to distinguish
         * "needs 8-byte read" from "needs 4-byte read" — snprintf
         * will respect the actual modifier in `spec` for output
         * formatting. */
        int is_ll = 0;
        for (size_t i = 1; i + 1 < sp; i++) {
            if (spec[i] == 'l' && spec[i+1] == 'l') { is_ll = 1; break; }
        }
        int is_double = (conv == 'f' || conv == 'F' || conv == 'e' ||
                         conv == 'E' || conv == 'g' || conv == 'G' ||
                         conv == 'a' || conv == 'A');

        uint32_t slot_addr = arg_addr + (uint32_t)(slot * 16);
        char piece[SPU_PRINTF_S_BUFCAP];

        if (conv == 's') {
            uint32_t s_addr = 0;
            if (sys_spu_thread_read_ls_word(thread_id, slot_addr, &s_addr) != 0) {
                snprintf(piece, sizeof(piece), "(spu-ls-read-fail)");
            } else if (s_addr == 0) {
                snprintf(piece, sizeof(piece), spec, "(null)");
            } else {
                char sbuf[SPU_PRINTF_S_BUFCAP];
                read_spu_ls_string(thread_id, s_addr, sbuf, sizeof(sbuf));
                snprintf(piece, sizeof(piece), spec, sbuf);
            }
        } else if (conv == 'c') {
            uint32_t v = 0;
            sys_spu_thread_read_ls_word(thread_id, slot_addr, &v);
            snprintf(piece, sizeof(piece), spec, (int)(v & 0xff));
        } else if (conv == 'p') {
            uint32_t v = 0;
            sys_spu_thread_read_ls_word(thread_id, slot_addr, &v);
            /* Print SPU pointers as 8-hex-digits with a 0x prefix so
             * the output is unambiguous; ignore caller's spec width
             * for %p (snprintf("%p", ptr) is implementation-defined
             * and not portable to a 32-bit SPU pointer anyway). */
            snprintf(piece, sizeof(piece), "0x%08x", (unsigned)v);
        } else if (is_double) {
            uint64_t v64 = 0;
            sys_spu_thread_read_ls_dword(thread_id, slot_addr, &v64);
            double d;
            memcpy(&d, &v64, sizeof(d));
            snprintf(piece, sizeof(piece), spec, d);
        } else if (is_ll) {
            uint64_t v64 = 0;
            sys_spu_thread_read_ls_dword(thread_id, slot_addr, &v64);
            if (conv == 'd' || conv == 'i') {
                snprintf(piece, sizeof(piece), spec, (long long)v64);
            } else {
                snprintf(piece, sizeof(piece), spec, (unsigned long long)v64);
            }
        } else {
            uint32_t v = 0;
            sys_spu_thread_read_ls_word(thread_id, slot_addr, &v);
            if (conv == 'd' || conv == 'i') {
                snprintf(piece, sizeof(piece), spec, (int)v);
            } else {
                snprintf(piece, sizeof(piece), spec, (unsigned)v);
            }
        }
        piece[sizeof(piece) - 1] = '\0';
        append(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, piece);
        slot++;
    }

    return (int)out_len;
}

/* ---- server thread ----------------------------------------------- *
 *
 * Loops forever receiving events from s_event_queue.  When an SPU
 * printf event arrives, sys_event_t.data_1 is the originating SPU
 * thread id and data_3 is the LS address of the saved-args block.
 * Path A hands the LS address to libc.sprx; Path B walks the args
 * here.  A non-zero return must be mailbox'd back to the SPU so its
 * `_spu_call_event_va_arg` can unblock — sysSpuThreadWriteMb does
 * that.
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

#if SPU_PRINTF_USE_SPRX_FORMATTER
        /* Path A — real-hardware-canonical.
         * Hand off to libc.sprx's spu_thread_printf which reads the
         * format string + args from SPU LS and produces the formatted
         * line on PPU stdio with full %d / %s / %lld support.
         *
         * NOTE: At the time of writing, RPCS3's HLE for this NID
         * (0xb1f4779d, sys_libc.spu_thread_printf) is unimplemented;
         * calling it logs `PPU: Unregistered function called` and
         * returns garbage / 0 with no output.  Pick the other branch
         * (default) when targeting RPCS3. */
        s32 sret = spu_thread_printf(thread_id, arg_addr);
        rc = sysSpuThreadWriteMb(thread_id, (u32)sret);
#else
        /* Path B — RPCS3 workaround with full format-string parsing.
         * The arg block at SPU LS offset arg_addr was laid down by
         * _spu_call_event_va_arg — first 4 bytes hold the LS address
         * of the format string, and each subsequent 16-byte slot holds
         * one vararg in its preferred-slot position.  `format_spu_printf`
         * walks the format string, pulls each spec's value out of LS
         * via syscall 182, and snprintfs into out_buf.  One fputs at
         * the end so concurrent server threads can't interleave. */
        char out_buf[SPU_PRINTF_OUT_BUFCAP];
        int n = format_spu_printf(thread_id, arg_addr, out_buf);
        if (n > 0) {
            fputs(out_buf, stdout);
        }
        rc = sysSpuThreadWriteMb(thread_id, 0);
#endif
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

    /* 16 KB stack: format_spu_printf keeps ~2 KB of buffers live
     * (out_buf + fmt + piece + sbuf), plus PPC64 ELFv1 linkage frames
     * and the sys_event_t struct.  4 KB (the old value) overflowed
     * and the handler crashed on the first event with fmt specifiers. */
    rc = sysThreadCreate(&s_handler_thread, spu_printf_server_entry, NULL,
                         priority, 16 * 1024, THREAD_JOINABLE,
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
