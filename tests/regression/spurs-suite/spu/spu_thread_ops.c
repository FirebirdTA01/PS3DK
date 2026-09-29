/* spurs-suite SPU thread operations, SPU side (row spu-thread-ops).
 * A plain SPU thread (no SPURS):
 *   1. publishes the LS addresses of ls_in / ls_out to the PPU's box
 *   2. waits for inbound mailbox word m1 (the PPU has written ls_in with
 *      sys_spu_thread_write_ls first)
 *   3. ls_out = ls_in + m1, then sends a user event on SPU port 1:
 *      data0 = ls_out & 0xffffff, data1 = m1
 *   4. waits for mailbox word m2 (the PPU has read ls_out with
 *      sys_spu_thread_read_ls, suspended and resumed the group) and
 *      exits with m2
 * arg1 = box EA. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>
#include "../spu_thread_ops.h"

volatile uint32_t ls_in __attribute__((aligned(16)));
volatile uint32_t ls_out __attribute__((aligned(16)));

int main(uint64_t box_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg2; (void)arg3; (void)arg4;
    __attribute__((aligned(16))) uint32_t buf[4];

    buf[0] = STO_MAGIC;
    buf[1] = (uint32_t)(uintptr_t)&ls_in;
    buf[2] = (uint32_t)(uintptr_t)&ls_out;
    buf[3] = 0;
    mfc_put(buf, box_ea, sizeof buf, 0, 0, 0);
    mfc_write_tag_mask(1u);
    mfc_read_tag_status_all();

    uint32_t m1 = spu_read_in_mbox();
    ls_out = ls_in + m1;
    sys_spu_thread_send_event(STO_PORT, ls_out & 0xffffffu, m1);

    uint32_t m2 = spu_read_in_mbox();
    sys_spu_thread_exit((int)m2);
}
