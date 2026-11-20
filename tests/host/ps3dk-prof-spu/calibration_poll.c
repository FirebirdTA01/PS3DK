/* Early calibration observations must bound a published CAL, without consuming
 * its slots or changing the later DRAIN_TB evidence. Uses the real producer. */
#include <stdio.h>
#include <string.h>
#include "model.h"

/* A baseline build has no poll API: it must fail the timing assertion, rather
 * than merely fail to link. Production builds do not use this weak declaration. */
extern void ps3dk_prof__psp_poll_calibrations(psp_ppu *) __attribute__((weak));

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void poll(run *r)
{
    if (ps3dk_prof__psp_poll_calibrations)
        ps3dk_prof__psp_poll_calibrations(&r->p);
}

static uint64_t (*real_ld64)(void *, uint64_t);
static uint64_t generation_ea, publication_tb;
static int generation_reads;
static uint64_t timed_ld64(void *ctx, uint64_t ea)
{
    uint64_t v = real_ld64(ctx, ea);
    if (ea == generation_ea && ++generation_reads == 2) {
        g_tb += 1000;
        publication_tb = g_tb;
    }
    return v;
}

static void flush(lane_h *h)
{
    uint16_t before = h->s.cal_answered;
    if (psp_spu_flush(&h->s) != PSP_OK) FAIL("producer flush");
    cal_note(h, before);
}

static uint64_t last_upper(run *r)
{
    for (size_t i = r->nch; i; --i)
        if (r->ch[i-1].type == 10 && r->ch[i-1].len == 44)
            return le64(r->ch[i-1].data + 36);
    FAIL("missing calibration bracket");
    return 0;
}

static void readonly_poll(run *r, lane_h *h)
{
    psp_lane *l = &r->p.lanes[h->idx];
    uint8_t control[256];
    uint64_t read = l->r, reservations = r->p.brackets_file;
    size_t chunks = r->nch;
    memcpy(control, g_mem + l->ea_ctl, sizeof control);
    poll(r);
    if (memcmp(control, g_mem + l->ea_ctl, sizeof control) || read != l->r ||
        reservations != r->p.brackets_file || chunks != r->nch)
        FAIL("calibration poll consumed, wrote, reserved or emitted");
}

static void scenario(int mode)
{
    run r;
    g_order_next = 1;
    run_init(&r, 64, 1, NULL);
    lane_h *h = lane_new(&r, 16, 0, 1);
    psp_lane *l = &r.p.lanes[h->idx];
    psp_drain(&r.p); /* declares the lane and asks for calibration */
    if (!l->out_id) FAIL("no request");
    if (mode == 1) readonly_poll(&r, h); /* an unanswered request proves nothing */
    zb(h, 0); ze(h, 0); flush(h);
    uint8_t saved[128];
    memcpy(saved, g_mem + l->ea_ctl + PSP_B, sizeof saved);
    if (mode == 2) /* coherent line, but a different answer identity */
        psp_be16(g_mem + l->ea_ctl + PSP_B_CAL_ANSWERED, (uint16_t)(l->out_id + 1));
    if (mode == 3) /* torn publication */
        psp_be64(g_mem + l->ea_ctl + PSP_B_GEN_END, l->good.gen + 100);
    if (mode == 4) /* matching answer but CAL lies beyond the observed prefix */
        psp_be64(g_mem + l->ea_ctl + PSP_B_WRITE_IDX, l->r + 2);
    uint64_t clock = g_tb;
    if (mode == 5) g_tb = l->out_tb0 - 10;
    if (mode == 6) {
        real_ld64 = r.m.ld64;
        generation_ea = l->ea_ctl + PSP_B_GEN;
        generation_reads = 0;
        publication_tb = 0;
        r.m.ld64 = timed_ld64;
    }
    if (mode != 1) readonly_poll(&r, h);
    uint64_t observed = g_tb;
    if (mode == 5) observed = g_tb = clock;
    if (mode == 6) r.m.ld64 = real_ld64;
    memcpy(g_mem + l->ea_ctl + PSP_B, saved, sizeof saved);
    g_tb += 100000; /* normal frame drain occurs much later */
    if (mode == 0) readonly_poll(&r, h); /* keep the first, tightest observation */
    psp_drain(&r.p);
    uint64_t upper = last_upper(&r);
    if ((mode == 0 || mode == 6) && upper > observed)
        FAIL("published answer: upper %llu exceeds observation %llu",
             (unsigned long long)upper, (unsigned long long)observed);
    if (mode != 0 && mode != 6 && upper <= observed)
        FAIL("invalid/unanswered observation was used (mode %d)", mode);
    if (mode == 6 && (!publication_tb || upper <= publication_tb))
        FAIL("upper timestamp precedes accepting publication read");
    uint16_t before = h->s.cal_answered;
    if (psp_spu_fini(&h->s) != PSP_OK) FAIL("fini");
    cal_note(h, before);
    if (psp_lane_end(&r.p, h->idx) != PSP_P_OK) FAIL("lane end");
    psp_seal(&r.p);
    check_streams(&r, "calibration-poll");
    if (check_order(&r, "calibration-poll", CK_RUN)) FAIL("order evidence changed");
    if (g_order_faults) FAIL("publication barrier missing");
    run_free(&r);
}

int main(void)
{
    for (int i = 0; i < 7; ++i) scenario(i);
    printf("calibration poll: 7 schedules, failures=%d\n", g_failures);
    return g_failures ? 1 : 0;
}
