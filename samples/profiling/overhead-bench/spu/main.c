/* overhead-bench SPU program: one thread, one lane.  arg0 ring EA, arg1 ring slots, arg2 control-block EA, arg3 the
 * EA of the PPU's ob_spu_res (common/ob_spu_res.h).  The writer attaches on DMA tag OB_WRITER_TAG; the replies use
 * OB_BENCH_TAG only, and only outside the timed blocks.  The decrementer is written once (dec_write) before the
 * blocks.  Command words from the inbound mailbox: 0 exit; else (seq << 16) | (case << 8) | tu (tu 0 on, 1 off;
 * common/ob_spu_res.h).  For each command the SPU runs its preconditions and the timed block, then replies with eight
 * words: d0, d1, rc, sseq delta, tail_lost delta, staged delta (two's complement), published (staged events cleared
 * by a successful flush), lost (older_lost delta), plus the command word it answers (w[8]) and the reset status
 * (w[9]: 1 when the decrementer write before the blocks returned success).  OFF blocks report zero ledgers. */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stdint.h>
#include <sys/spu_thread.h>

#include "../common/ob_cases.h"
#include "../common/ob_spu_res.h"
#include "prof_spu_lane.h"

typedef struct ob_d2 {
    uint32_t d0, d1;
} ob_d2;
void bench_spu_on_pairs(uint32_t n, ob_d2 *t);
void bench_spu_on_stage(uint32_t k);
int bench_spu_on_flush(ob_d2 *t);
void bench_spu_off_pairs(uint32_t n, ob_d2 *t);
void bench_spu_off_stage(uint32_t k);
int bench_spu_off_flush(ob_d2 *t);

ps3dk_prof_spu_writer ob_w __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN)));
static uint8_t g_stage[PS3DK_PROF_SPU_STAGING_BYTES(OB_LS_CAP)] __attribute__((aligned(16)));
static ob_spu_res g_res;
static uint64_t g_res_ea;
static uint32_t g_reply, g_dec_reset;

/* words PUT, then the fenced seq PUT on the same tag, then wait for the tag (no bench DMA stays in flight) */
static void reply(const uint32_t w[8], uint32_t cmd)
{
    for (unsigned i = 0; i < 8; ++i)
        g_res.w[i] = w[i];
    g_res.w[8] = cmd;
    g_res.w[9] = g_dec_reset;
    g_res.seq = ++g_reply;
    mfc_put(&g_res, g_res_ea, 64, OB_BENCH_TAG, 0, 0);
    mfc_putf(&g_res.seq, g_res_ea + 124u, 4, OB_BENCH_TAG, 0, 0);
    mfc_write_tag_mask(1u << OB_BENCH_TAG);
    mfc_read_tag_status_all();
}

/* the ring is filled (no PPU drain) by flushing staged marks until a flush writes no slot */
static void fill_ring(void)
{
    for (unsigned i = 0; i < 100000u; ++i) {
        uint64_t w = ob_w.s.w;
        bench_spu_on_stage(16);
        ps3dk_prof_spu_flush(&ob_w);
        if (ob_w.s.w == w)
            return;
    }
}

static void run(uint32_t cmd, uint32_t c, uint32_t tu)
{
    ob_d2 t = {0, 0};
    int rc = 0, on = tu == 0;
    uint32_t n = c < OB_CASES ? ob_case_table[c].n : 0u, r[8];
    if (on && c != OB_S_STAGEFULL && c != OB_S_BACKPRESS)
        ps3dk_prof_spu_flush(&ob_w); /* staging 0 (the PPU drains between blocks) */
    if (c >= OB_S_FLUSH0 && c <= OB_S_FLUSH128)
        (on ? bench_spu_on_stage : bench_spu_off_stage)(n);
    if (on && (c == OB_S_STAGEFULL || c == OB_S_BACKPRESS)) {
        fill_ring();
        if (c == OB_S_STAGEFULL)
            bench_spu_on_stage(OB_LS_CAP - ob_w.s.n);
        else
            bench_spu_on_stage(n);
    }
    uint64_t s0 = ob_w.s.seq, l0 = ob_w.s.tail_lost, o0 = ob_w.s.older_lost;
    uint32_t n0 = ob_w.s.n;
    if (c == OB_S_PAIR || c == OB_S_STAGEFULL)
        (on ? bench_spu_on_pairs : bench_spu_off_pairs)(n, &t);
    else
        rc = (on ? bench_spu_on_flush : bench_spu_off_flush)(&t);
    r[0] = t.d0;
    r[1] = t.d1;
    r[2] = (uint32_t)rc;
    r[3] = on ? (uint32_t)(ob_w.s.seq - s0) : 0u;
    r[4] = on ? (uint32_t)(ob_w.s.tail_lost - l0) : 0u;
    r[5] = on ? (uint32_t)((int32_t)ob_w.s.n - (int32_t)n0) : 0u;
    r[6] = on && rc == 0 && (c >= OB_S_FLUSH0 && c <= OB_S_BACKPRESS) && ob_w.s.older_lost == o0 ? n0 - ob_w.s.n : 0u;
    r[7] = on ? (uint32_t)(ob_w.s.older_lost - o0) : 0u;
    reply(r, cmd);
}

int main(uint64_t ring, uint64_t slots, uint64_t ctl, uint64_t res_ea)
{
    uint32_t r[8] = {0};
    g_res_ea = res_ea;
    int arc = ps3dk_prof_spu_attach(&ob_w, ring, (uint32_t)slots, ctl, OB_WRITER_TAG, g_stage, OB_LS_CAP,
                                    PS3DK_PROF_SPU_OWN_DEC);
    int drc = arc == 0 ? ps3dk_prof_spu_dec_write(&ob_w, 0xFFFFFFFFu) : -1;
    g_dec_reset = drc == 0;
    r[0] = (uint32_t)arc;
    r[1] = (uint32_t)drc;
    reply(r, 0);
    for (;;) {
        uint32_t w = spu_read_in_mbox();
        if (w == OB_SPU_EXIT)
            break;
        run(w, (w >> 8) & 0xFFu, w & 0xFFu);
    }
    ps3dk_prof_spu_flush(&ob_w);
    r[0] = (uint32_t)ps3dk_prof_spu_fini(&ob_w);
    r[1] = 0;
    reply(r, OB_SPU_EXIT);
    sys_spu_thread_exit(0);
    return 0;
}
