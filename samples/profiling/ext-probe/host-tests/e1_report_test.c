/* e1_report_check (source/e1_report.h): every payload state the SPU core can send is accepted, and every damage
 * (one field, or a state the producer cannot reach) is refused with the NAMED rule.  Built with the target's
 * descriptor count (E1_DESC_COUNT 3).  HOST ONLY.  Prints one line per case and "E1-REPORT PASS <n>" /
 * "E1-REPORT FAIL <k> of <n>". */
#include <stdio.h>
#include <string.h>

#include "../source/e1_report.h"

static int g_n, g_bad;

typedef struct {
    uint32_t call, rc, ev1, ev2, dec, dec_rc, ridx, hi, lo, desc;
} pl;

static void make(uint32_t *w, uint32_t kind, pl p)
{
    memset(w, 0, 32 * sizeof *w);
    w[E1_W_MAGIC] = E1_MAGIC;
    w[E1_W_KIND] = kind;
    w[E1_W_SEQ] = kind;
    w[E1_W_FAIL_CALL] = p.call;
    w[E1_W_FAIL_RC] = p.rc;
    w[E1_W_EV1] = p.ev1;
    w[E1_W_EV2] = p.ev2;
    w[E1_W_DEC_STATE] = p.dec;
    w[E1_W_DEC_RC] = p.dec_rc;
    w[E1_W_RIDX_STATE] = p.ridx;
    w[E1_W_RIDX_HI] = p.hi;
    w[E1_W_RIDX_LO] = p.lo;
    w[E1_W_DESC] = p.desc;
}

static void expect(const char *name, const uint32_t *w, uint32_t seq, int want)
{
    int got = e1_report_check(w, seq);
    g_n++;
    if (got != want) {
        g_bad++;
        printf("FAIL %s: rule %d, want %d\n", name, got, want);
    } else {
        printf("ok   %s: rule %d\n", name, got);
    }
}

#define EF 0xFFFFFFEBu /* -21 EFAULT */
#define EA 0xFFFFFFECu /* -20 EAGAIN */
#define EI 0xFFFFFFFBu /* -5 EINVAL */
#define EX 0xFFFFFFEAu /* -22 EEXHAUSTED */
#define D E1_DESC_COUNT
#define V E1_RIDX_VALID
#define OK E1_DEC_OK

int main(void)
{
    uint32_t w[32];
    _Static_assert(E1_DESC_COUNT == 3u, "the unit test pins the target descriptor count");
    /* ---- every producer-possible state (accepted) ---- */
    const pl k1_clean = {0, 0, 132, 0, 0, 0, 0, 0, 0, D};
    const pl k1_attach = {E1_CALL_ATTACH, EI, 0, 0, 0, 0, 0, 0, 0, 0};
    const pl k1_flush_first = {E1_CALL_FLUSH, EF, 16, 0, 0, 0, 0, 0, 0, D};
    const pl k1_flush_last = {E1_CALL_FLUSH, EX, 132, 0, 0, 0, 0, 0, 0, D};
    const pl k1_event = {E1_CALL_EVENT, EF, 49, 0, 0, 0, 0, 0, 0, D};
    const pl k2_clean = {0, 0, 132, 132, OK, 0, V, 0, 0x88, D};
    const pl k2_clean_ridx0 = {0, 0, 132, 132, OK, 0, V, 0, 0, D}; /* a valid read_idx of 0 is not "unavailable" */
    const pl k2_go = {E1_CALL_GO, 0xFFFFFFFFu, 132, 0, 0, 0, 0, 0, 0, D};
    const pl k2_flush_a = {E1_CALL_FLUSH, EF, 132, 0, 0, 0, 0, 0, 0, D};
    const pl k2_flush_b = {E1_CALL_FLUSH, EF, 132, 0, 0, 0, V, 0, 0x88, D};
    const pl k2_flush_c = {E1_CALL_FLUSH, EF, 132, 66, OK, 0, V, 1, 0x88, D};
    const pl k2_dec = {E1_CALL_DEC, EF, 132, 0, E1_DEC_FAILED, EF, V, 0, 0x88, D};
    const pl k2_dec_eagain = {E1_CALL_DEC, EA, 132, 0, E1_DEC_FAILED, EA, V, 0, 0x88, D};
    const pl k2_event = {E1_CALL_EVENT, EF, 132, 16, OK, 0, V, 0, 0x88, D};
    const pl k2_fini = {E1_CALL_FINI, EF, 132, 132, OK, 0, V, 0, 0x88, D};
    struct {
        const char *n;
        uint32_t kind;
        pl p;
    } good[] = {{"k1-clean", 1, k1_clean}, {"k1-attach", 1, k1_attach}, {"k1-flush-first", 1, k1_flush_first},
                {"k1-flush-last-exhausted", 1, k1_flush_last}, {"k1-event", 1, k1_event}, {"k2-clean", 2, k2_clean},
                {"k2-clean-ridx-0", 2, k2_clean_ridx0}, {"k2-go", 2, k2_go}, {"k2-flush-first-post-go", 2, k2_flush_a},
                {"k2-flush-after-eagain", 2, k2_flush_b}, {"k2-flush-epoch2", 2, k2_flush_c},
                {"k2-dec-efault", 2, k2_dec}, {"k2-dec-eagain-twice", 2, k2_dec_eagain}, {"k2-event", 2, k2_event},
                {"k2-fini", 2, k2_fini}};
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
        make(w, good[i].kind, good[i].p);
        expect(good[i].n, w, good[i].kind, 0);
    }

    /* ---- review 10:25 counterexamples ---- */
    make(w, 1, k1_clean); w[E1_W_FAIL_CALL] = E1_CALL_GO; w[E1_W_FAIL_RC] = 0xFFFFFFFFu; expect("k1 call go (post-GO only)", w, 1, 19);
    make(w, 1, k1_attach); w[E1_W_EV1] = 132; w[E1_W_DESC] = D; expect("attach failure with ev1 132", w, 1, 17);
    make(w, 2, k2_fini); w[E1_W_EV2] = 0; expect("fini with ev2 0", w, 2, 14);

    /* ---- framing ---- */
    make(w, 1, k1_clean); w[E1_W_MAGIC] ^= 1; expect("magic", w, 1, 1);
    make(w, 1, k1_clean); expect("k1 as seq 2", w, 2, 2);
    make(w, 2, k2_clean); expect("k2 as seq 1", w, 1, 2);
    make(w, 1, k1_clean); w[E1_W_SEQ] = 2; expect("seq word != kind", w, 1, 2);
    make(w, 1, k1_clean); w[E1_W_KIND] = 3; w[E1_W_SEQ] = 3; expect("kind 3", w, 3, 2);
    make(w, 1, k1_clean); w[E1_W_FAIL_CALL] = 7; w[E1_W_FAIL_RC] = EF; expect("call 7", w, 1, 3);
    make(w, 2, k2_clean); w[E1_W_DEC_STATE] = 3; expect("dec state 3", w, 2, 3);
    make(w, 2, k2_clean); w[E1_W_RIDX_STATE] = 2; expect("ridx state 2", w, 2, 3);
    for (unsigned i = E1_W_COUNT; i < 32u; i++) {
        char n[32];
        make(w, 2, k2_clean);
        w[i] = 1;
        snprintf(n, sizeof n, "tail word %u", i);
        expect(n, w, 2, 6);
    }
    /* ---- rc: zero, outside int32 sense, not that call's result ---- */
    make(w, 1, k1_attach); w[E1_W_FAIL_RC] = 0; expect("error with rc 0", w, 1, 4);
    make(w, 1, k1_clean); w[E1_W_FAIL_RC] = 5; expect("rc without error", w, 1, 4);
    make(w, 1, k1_flush_first); w[E1_W_FAIL_RC] = 0x80000000u; expect("flush rc INT32_MIN", w, 1, 4);
    make(w, 1, k1_flush_first); w[E1_W_FAIL_RC] = 5; expect("flush rc +5", w, 1, 4);
    make(w, 1, k1_flush_first); w[E1_W_FAIL_RC] = 0xFFFFFFFFu; expect("flush rc -1", w, 1, 4);
    make(w, 1, k1_attach); w[E1_W_FAIL_RC] = EX; expect("attach EEXHAUSTED", w, 1, 4);
    make(w, 1, k1_attach); w[E1_W_FAIL_RC] = EA; expect("attach EAGAIN", w, 1, 4);
    make(w, 1, k1_event); w[E1_W_FAIL_RC] = EI; expect("event EINVAL", w, 1, 4);
    make(w, 2, k2_go); w[E1_W_FAIL_RC] = EF; expect("go EFAULT", w, 2, 4);
    make(w, 2, k2_dec); w[E1_W_FAIL_RC] = EX; w[E1_W_DEC_RC] = EX; expect("dec EEXHAUSTED", w, 2, 4);
    /* ---- kind 1 states ---- */
    make(w, 1, k1_clean); w[E1_W_EV2] = 1; expect("k1 ev2", w, 1, 7);
    make(w, 1, k1_clean); w[E1_W_DEC_STATE] = OK; expect("k1 dec ok", w, 1, 7);
    make(w, 1, k1_clean); w[E1_W_DEC_RC] = 1; expect("k1 dec rc", w, 1, 11);
    make(w, 1, k1_clean); w[E1_W_RIDX_STATE] = V; expect("k1 ridx valid", w, 1, 7);
    make(w, 1, k1_clean); w[E1_W_RIDX_LO] = 1; expect("k1 unavailable ridx lo", w, 1, 13);
    make(w, 1, k1_clean); w[E1_W_RIDX_HI] = 1; expect("k1 unavailable ridx hi", w, 1, 13);
    make(w, 1, k1_clean); w[E1_W_FAIL_CALL] = E1_CALL_DEC; w[E1_W_FAIL_RC] = EF; expect("k1 call dec", w, 1, 19);
    make(w, 1, k1_clean); w[E1_W_FAIL_CALL] = E1_CALL_FINI; w[E1_W_FAIL_RC] = EF; expect("k1 call fini", w, 1, 19);
    make(w, 1, k1_clean); w[E1_W_EV1] = 131; expect("k1 clean short", w, 1, 8);
    make(w, 1, k1_clean); w[E1_W_DESC] = 2; expect("k1 clean desc 2", w, 1, 8);
    make(w, 1, k1_attach); w[E1_W_DESC] = D; expect("attach failure with desc", w, 1, 17);
    make(w, 1, k1_flush_first); w[E1_W_EV1] = 15; expect("k1 flush before 16 events", w, 1, 18);
    make(w, 1, k1_flush_first); w[E1_W_EV1] = 133; expect("k1 flush ev1 133", w, 1, 18);
    make(w, 1, k1_event); w[E1_W_EV1] = 0; expect("k1 event with no events", w, 1, 18);
    make(w, 1, k1_event); w[E1_W_DESC] = 0; expect("k1 event desc 0", w, 1, 18);
    /* ---- kind 2 states ---- */
    make(w, 2, k2_go); w[E1_W_FAIL_CALL] = E1_CALL_ATTACH; w[E1_W_FAIL_RC] = EI; expect("k2 attach", w, 2, 9);
    make(w, 2, k2_clean); w[E1_W_EV1] = 131; expect("k2 ev1 short", w, 2, 10);
    make(w, 2, k2_clean); w[E1_W_DESC] = 2; expect("k2 desc 2", w, 2, 10);
    make(w, 2, k2_clean); w[E1_W_DEC_RC] = 1; expect("k2 ok dec rc", w, 2, 11);
    make(w, 2, k2_go); w[E1_W_DEC_RC] = 1; expect("k2 not-attempted dec rc", w, 2, 11);
    make(w, 2, k2_go); w[E1_W_EV2] = 1; expect("go with ev2", w, 2, 15);
    make(w, 2, k2_go); w[E1_W_DEC_STATE] = OK; expect("go with dec ok", w, 2, 15);
    make(w, 2, k2_go); w[E1_W_RIDX_STATE] = V; w[E1_W_RIDX_LO] = 0x88; expect("go with ridx", w, 2, 15);
    make(w, 2, k2_flush_a); w[E1_W_RIDX_LO] = 1; expect("ridx unavailable nonzero", w, 2, 13);
    make(w, 2, k2_flush_c); w[E1_W_EV2] = 15; expect("epoch-2 flush before 16 events", w, 2, 20);
    make(w, 2, k2_flush_c); w[E1_W_RIDX_STATE] = 0; w[E1_W_RIDX_HI] = 0; w[E1_W_RIDX_LO] = 0; expect("epoch-2 flush without ridx", w, 2, 20);
    make(w, 2, k2_flush_c); w[E1_W_DEC_STATE] = 0; expect("epoch-2 events without dec", w, 2, 20);
    make(w, 2, k2_dec); w[E1_W_DEC_STATE] = OK; w[E1_W_DEC_RC] = 0; expect("call dec, dec ok", w, 2, 12);
    make(w, 2, k2_dec); w[E1_W_DEC_RC] = EA; expect("dec rc != fail rc", w, 2, 12);
    make(w, 2, k2_dec); w[E1_W_RIDX_STATE] = 0; w[E1_W_RIDX_LO] = 0; expect("dec without ridx", w, 2, 12);
    make(w, 2, k2_dec); w[E1_W_EV2] = 16; expect("dec failed after epoch-2 events", w, 2, 12);
    make(w, 2, k2_flush_c); w[E1_W_DEC_STATE] = E1_DEC_FAILED; w[E1_W_DEC_RC] = EF; expect("dec failed, call flush", w, 2, 20);
    make(w, 2, k2_event); w[E1_W_EV2] = 0; expect("k2 event with no epoch-2 events", w, 2, 21);
    make(w, 2, k2_event); w[E1_W_DEC_STATE] = 0; expect("k2 event without dec", w, 2, 21);
    make(w, 2, k2_clean); w[E1_W_EV2] = 131; expect("clean ev2 short", w, 2, 14);
    make(w, 2, k2_clean); w[E1_W_RIDX_STATE] = 0; w[E1_W_RIDX_LO] = 0; expect("clean ridx unavailable", w, 2, 14);
    make(w, 2, k2_clean); w[E1_W_DEC_STATE] = 0; expect("clean dec not attempted", w, 2, 14);
    make(w, 2, k2_fini); w[E1_W_RIDX_STATE] = 0; w[E1_W_RIDX_LO] = 0; expect("fini without ridx", w, 2, 14);

    /* ---- P-EA32: the report-line EAs pack exactly, or are refused (never narrowed) ---- */
    {
        uint64_t v = 7;
        ++g_n;
        if (e1_pack_report_eas(0x10000080u, 0x10000100u, &v) != 0 || v != 0x1000008010000100ull) {
            ++g_bad;
            printf("FAIL ea32: 32-bit EAs pack\n");
        } else
            printf("PASS ea32: 32-bit EAs pack\n");
        ++g_n;
        if (e1_pack_report_eas(0xffffff80u, 0xffffff00u, &v) != 0 || v != 0xffffff80ffffff00ull) {
            ++g_bad;
            printf("FAIL ea32: the highest 32-bit EAs pack\n");
        } else
            printf("PASS ea32: the highest 32-bit EAs pack\n");
        v = 7;
        ++g_n;
        if (e1_pack_report_eas(0x100000080ull, 0x10000100u, &v) != -1 || v != 7) {
            ++g_bad;
            printf("FAIL ea32: a 64-bit payload EA is refused, output untouched\n");
        } else
            printf("PASS ea32: a 64-bit payload EA is refused, output untouched\n");
        ++g_n;
        if (e1_pack_report_eas(0x10000080u, 0x100000000ull, &v) != -1 || v != 7) {
            ++g_bad;
            printf("FAIL ea32: a 64-bit commit EA is refused, output untouched\n");
        } else
            printf("PASS ea32: a 64-bit commit EA is refused, output untouched\n");
    }

    if (g_bad)
        printf("E1-REPORT FAIL %d of %d\n", g_bad, g_n);
    else
        printf("E1-REPORT PASS %d\n", g_n);
    return g_bad ? 1 : 0;
}
