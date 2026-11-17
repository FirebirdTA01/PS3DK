/* Hand-derived owner-buffer counts. No GPU execution is modelled here. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#if defined(__has_include)
#if __has_include("fifo_account.h")
#define HAVE_FIFO_ACCOUNT 1
#include "fifo_account.h"
#endif
#endif
#ifndef HAVE_FIFO_ACCOUNT
int main(void) { fputs("FAIL: owner FIFO accounting is unavailable\n", stderr); return 1; }
#else
static fifo_account fresh(void)
{
    fifo_account s;
    fifo_account_init(&s, 0x1000, 0x1100, 0x1000, 0x1000);
    return s;
}
static void wrap(fifo_account *s, uint32_t tail)
{
    for (unsigned k = 1; k <= 6; ++k) fifo_account_wrap(s, k, 0x1000, tail);
}
static void counts(fifo_account_row r, uint64_t p, uint64_t u, uint64_t w)
{
    assert(r.reasons == 0 && r.prepared == p && r.published == u && r.wraps == w);
}
int main(void)
{
    fifo_account s = fresh();
    fifo_account_row r;
    /* Removing separate publication tracking double-counts repeated PUTs. */
    fifo_account_begin(&s, 1, 7, 1, 0x1000);
    fifo_account_prepare(&s, 0x1040);
    fifo_account_publish(&s, 0x1010);
    fifo_account_publish(&s, 0x1010);
    fifo_account_publish(&s, 0x1020);
    r = fifo_account_end(&s, 1, 7, 1, 0x1040);
    counts(r, 64, 32, 0);
    /* A frame may publish bytes prepared in an earlier frame. */
    fifo_account_begin(&s, 1, 8, 1, 0x1040);
    fifo_account_publish(&s, 0x1040);
    counts(fifo_account_end(&s, 1, 8, 1, 0x1040), 0, 32, 0);
    /* Early wraps leave holes. Charge actual bytes + one JUMP per lap. */
    s = fresh();
    fifo_account_begin(&s, 1, 0, 1, 0x1000);
    wrap(&s, 0x1040);
    wrap(&s, 0x1020);
    fifo_account_prepare(&s, 0x1010);
    fifo_account_publish(&s, 0x1008);
    counts(fifo_account_end(&s, 1, 0, 1, 0x1010), 120, 112, 2);
    /* JUMP exists at phase1 BEGIN but is not published until phase2. */
    s = fresh();
    fifo_account_wrap(&s, 1, 0x1000, 0x1020);
    assert(s.prepared == 32 && s.published == 0);
    fifo_account_wrap(&s, 2, 0x1000, 0x1020);
    assert(s.prepared == 36 && s.published == 32);
    fifo_account_wrap(&s, 3, 0x1000, 0x1020);
    fifo_account_wrap(&s, 4, 0x1000, 0x1020);
    assert(s.prepared == 36 && s.published == 36);
    /* Missing phases, backwards cursors and unproven callbacks never infer laps. */
    for (unsigned mode = 0; mode != 3; ++mode) {
        s = fresh(); fifo_account_begin(&s, 1, 2, 1, 0x1000);
        fifo_account_prepare(&s, 0x1040);
        if (mode == 0) fifo_account_wrap(&s, 4, 0x1000, 0x1040);
        if (mode == 1) fifo_account_prepare(&s, 0x1000);
        if (mode == 2) fifo_account_invalidate(&s, FIFO_ACCOUNT_FLOW);
        r = fifo_account_end(&s, 1, 2, 1, 0x1040);
        assert(r.reasons && !r.prepared && !r.published && !r.wraps);
    }
    s = fresh(); fifo_account_begin(&s, 1, 2, 1, 0x1000);
    for (unsigned k = 1; k <= 6; ++k)
        if (k != 3) fifo_account_wrap(&s, k, 0x1000, 0x1040);
    r = fifo_account_end(&s, 1, 2, 1, 0x1000);
    assert((r.reasons & FIFO_ACCOUNT_BOUNDARY) && !r.prepared && !r.wraps);
    /* Window/adoption and owner mismatch need unavailable evidence. */
    s = fresh(); fifo_account_begin(&s, 1, 3, 1, 0x1000);
    r = fifo_account_end(&s, 1, 3, 2, 0x1010);
    assert(r.reasons & FIFO_ACCOUNT_WINDOW);
    s = fresh(); r = fifo_account_end(&s, 1, 3, 1, 0x1010);
    assert(r.reasons & FIFO_ACCOUNT_BOUNDARY);
    s = fresh(); fifo_account_begin(&s, 1, 3, 1, 0x1000);
    r = fifo_account_end(&s, 2, 3, 1, 0x1010);
    assert(r.reasons & FIFO_ACCOUNT_TOKEN);
    /* Saturation never wraps into plausible small exact values. */
    s = fresh(); s.prepared = UINT64_MAX - 3;
    fifo_account_begin(&s, 1, 3, 1, 0x1000);
    r = fifo_account_end(&s, 1, 3, 1, 0x1004);
    assert((r.reasons & FIFO_ACCOUNT_SATURATED) && !r.prepared);
    /* Publishing beyond prepared or backwards cannot create exact bytes. */
    s = fresh(); fifo_account_begin(&s, 1, 4, 1, 0x1000);
    fifo_account_publish(&s, 0x1004);
    assert(fifo_account_end(&s, 1, 4, 1, 0x1000).reasons);
    s = fresh(); fifo_account_begin(&s, 1, 4, 1, 0x1000);
    fifo_account_prepare(&s, 0x1010); fifo_account_publish(&s, 0x1008);
    fifo_account_publish(&s, 0x1004);
    assert(fifo_account_end(&s, 1, 4, 1, 0x1010).reasons);
    /* Encode exact counters rather than host padding or endian layout. */
    s = fresh(); fifo_account_begin(&s, 1, 0, 1, 0x1000);
    r = fifo_account_end(&s, 1, 0, 1, 0x1010);
    r.seq = 9;
    uint8_t wire[80];
    fifo_account_encode(wire, &r, 1, 1, 1026);
    assert(wire[0] == 1 && wire[2] == 3 && wire[40] == 16 && wire[64] == 9);
    for (unsigned i = 72; i != 80; ++i) assert(wire[i] == 0);
    puts("FIFO accounting controls PASS");
    return 0;
}
#endif
