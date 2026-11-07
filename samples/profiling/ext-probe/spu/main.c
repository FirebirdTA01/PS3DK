/* E1 SPU program, target binding (TARGET ONLY; the host tests bind e1_spu_core.c to a model instead).
 * Arguments: arg0 = the lane ring EA, arg1 = (ring_slots << 32) | 0, arg2 = the lane control-block EA, arg3 =
 * (payload EA << 32) | commit EA (LV2 process addresses below 4 GiB, checked by the PPU; 128-aligned).  Reports follow the W probe's
 * sequence exactly (clock-calibration/spu/main.c): PAYLOAD put, wait-all, COMMIT put, wait-all, on E1_REPORT_TAG,
 * with the caller's tag mask saved and restored.  No other decrementer user.  The program ends with an explicit
 * sys_spu_thread_exit(0) (libsputhread): after an MFC tag-status sequence it writes the exit code to the outbound
 * mailbox and executes stop 0x102 (the LV2 thread-exit trap).  Returning from main instead ends in
 * the C library's _exit, which executes stop 0x2000, not that trap. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include <sys/spu_thread.h>

#include "e1_spu_core.h"

static ps3dk_prof_spu_writer g_writer __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t g_staging[PS3DK_PROF_SPU_STAGING_BYTES(E1_LS_CAP)] __attribute__((aligned(16)));
static e1_line g_commit; /* the LS source of the COMMIT put */

typedef struct {
    uint64_t payload_ea, commit_ea;
} target_ctx;

static uint32_t t_read_in_mbox(void *ctx)
{
    (void)ctx;
    return spu_read_in_mbox();
}

static void t_report(void *ctx, const e1_line *payload, uint32_t seq)
{
    const target_ctx *t = ctx;
    uint32_t saved = spu_readch(MFC_RdTagMask);
    __asm__ volatile("" ::: "memory");
    mfc_put((volatile void *)(uintptr_t)payload, t->payload_ea, sizeof *payload, E1_REPORT_TAG, 0, 0);
    mfc_write_tag_mask(1u << E1_REPORT_TAG);
    mfc_read_tag_status_all(); /* the payload is in main memory before the commit is sent */
    __asm__ volatile("" ::: "memory");
    g_commit.w[0] = seq;
    __asm__ volatile("" ::: "memory");
    mfc_put(&g_commit, t->commit_ea, sizeof g_commit, E1_REPORT_TAG, 0, 0);
    mfc_write_tag_mask(1u << E1_REPORT_TAG);
    mfc_read_tag_status_all();
    __asm__ volatile("" ::: "memory");
    mfc_write_tag_mask(saved);
}

int main(uint64_t ea_ring, uint64_t slots_arg, uint64_t ea_ctl, uint64_t report_eas)
{
    static target_ctx t;
    static const e1_spu_port port = {&t, t_read_in_mbox, t_report};
    t.payload_ea = report_eas >> 32;
    t.commit_ea = report_eas & 0xffffffffu;
    e1_spu_run(&port, &g_writer, g_staging, ea_ring, (uint32_t)(slots_arg >> 32), ea_ctl);
    sys_spu_thread_exit(0);
}
