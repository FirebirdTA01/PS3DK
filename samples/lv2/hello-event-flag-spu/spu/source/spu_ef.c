/* hello-event-flag-spu — SPU side.
 *
 * Wakes up, reads (request, event-flag-id) pairs from signal
 * notification channels 1 and 2, dispatches the request to the
 * matching sys_event_flag_set_bit variant, and exits cleanly when the
 * master sends SPU_REQ_OP_QUIT.
 *
 * Linked against $PS3DK/spu/lib/libsputhread.a — that library supplies
 * the LV2 syscalls we invoke (sys_event_flag_set_bit*, sys_spu_thread_exit,
 * sys_spu_thread_group_exit) plus the corresponding stop-and-signal
 * channel protocol (stop 0x101 / 0x102) the LV2 kernel recognises as
 * thread-exit traps.
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/event_flag.h>

#include "spu_ef.h"

/* Read SNR1 / SNR2 via the standard spu intrinsic.  Each read blocks
 * until the PPU pushes a value through sys_spu_thread_write_snr().
 * Channel ids match the SDK header constants. */
static inline uint32_t read_snr1(void) { return spu_read_signal1(); }
static inline uint32_t read_snr2(void) { return spu_read_signal2(); }

/* Drop into the LV2 thread-exit trap with `code` in the kernel's
 * status word.  Marked noreturn upstream — wrapping it locally keeps
 * call sites readable. */
static __attribute__((noreturn)) void worker_quit(int code)
{
    sys_spu_thread_exit(code);
}

/* Group-level abort: signals every other SPU in the group to stop too.
 * Used when the PPU sends an opcode the SPU doesn't understand — that
 * is a programming error and the group can't usefully continue. */
static __attribute__((noreturn)) void group_abort(int code)
{
    sys_spu_thread_group_exit(code);
}

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4;

    for (;;) {
        uint32_t req = read_snr1();
        uint32_t op  = SPU_REQ_OPCODE(req);
        uint32_t bit = SPU_REQ_BITIDX(req);

        if (op == SPU_REQ_OP_QUIT) {
            worker_quit(0);
        }

        /* Both set-bit variants need the event-flag-id from SNR2. */
        sys_event_flag_t ef = (sys_event_flag_t)read_snr2();

        int rc;
        switch (op) {
        case SPU_REQ_OP_SET_BIT:
            rc = sys_event_flag_set_bit(ef, bit);
            break;
        case SPU_REQ_OP_SET_BIT_IMP:
            rc = sys_event_flag_set_bit_impatient(ef, bit);
            break;
        default:
            /* Unknown opcode is a master-side bug; tear the group
             * down so the PPU's join() receives a non-zero status
             * instead of silently hanging on the next read_snr. */
            group_abort(-1);
        }

        if (rc != CELL_OK) {
            group_abort(rc);
        }
    }
}
