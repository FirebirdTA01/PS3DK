#include "ob_cases.h"

#include <stdio.h>
#include <string.h>

const ob_case ob_case_table[OB_CASES] = {
    [OB_P_HOT] = {"P-hot", 256u, 1, 0},
    [OB_P_NEST8] = {"P-nest8", 32u, 1, 0},
    [OB_P_COLD] = {"P-cold", 1u, 1, 0},
    /* n: ring slots to fill.  The host ledger model (A1 dev2) showed the 4096th slot record dropped on an empty
     * 4096-record ring after 4095 slots: the ring takes 4095 records, so the prefill is 1365 pairs = 4095 slots. */
    [OB_P_PREFILL] = {"P-prefill", OB_RING_RECORDS - 1u, 0, 0},
    [OB_P_DROP] = {"P-drop", 256u, 1, 0},
    [OB_P_FRAME] = {"P-frame", 256u, 1, 0},
    [OB_P_EMPTY] = {"P-empty", 0u, 1, 0},
    [OB_S_PAIR] = {"S-pair", 64u, 1, 1},
    [OB_S_FLUSH0] = {"S-flush-0", 0u, 1, 1},
    [OB_S_FLUSH1] = {"S-flush-1", 1u, 1, 1},
    [OB_S_FLUSH16] = {"S-flush-16", 16u, 1, 1},
    [OB_S_FLUSH128] = {"S-flush-128", 128u, 1, 1},
    [OB_S_STAGEFULL] = {"S-stagefull", 64u, 1, 1},
    [OB_S_BACKPRESS] = {"S-backpressure", 16u, 1, 1}, /* n: events staged when the flush meets the full ring */
};

ob_ledger ob_planned(int c)
{
    ob_ledger l;
    memset(&l, 0, sizeof l);
    uint32_t n = ob_case_table[c].n;
    switch (c) {
    /* a zone pair takes OB_PAIR_SEQ sequence numbers (ring slots): a BEGIN takes two, an END one (the host ledger
     * model measured it against the runtime core: dev1 in the A1 packet) */
    case OB_P_HOT:
        l.seq = OB_PAIR_SEQ * (uint64_t)n, l.depth = 1;
        break;
    case OB_P_NEST8:
        l.seq = 8ull * OB_PAIR_SEQ * n, l.depth = 8;
        break;
    case OB_P_COLD:
        l.seq = OB_PAIR_SEQ, l.regs = 1, l.depth = 1;
        break;
    case OB_P_PREFILL:
        l.seq = n, l.depth = 1; /* exactly the ring's capacity in slots, nothing dropped */
        break;
    case OB_P_DROP:
        l.seq = OB_PAIR_SEQ * (uint64_t)n, l.dropped = OB_PAIR_SEQ * (uint64_t)n, l.depth = 1; /* produced 0 */
        break;
    case OB_P_FRAME:
        l.seq = n; /* one record per frame mark (the host model checks the format's count) */
        break;
    case OB_S_PAIR:
        l.sseq = 2ull * n, l.staged = 2 * (int64_t)n;
        break;
    case OB_S_FLUSH0:
    case OB_S_FLUSH1:
    case OB_S_FLUSH16:
    case OB_S_FLUSH128:
        l.published = n, l.staged = -(int64_t)n; /* n: the staging level k before the flush */
        break;
    case OB_S_STAGEFULL:
        l.sseq = 2ull * n, l.tail_lost = 2ull * n; /* staged unchanged, no slot write */
        break;
    case OB_S_BACKPRESS:
        /* the host model of the writer (A1 dev): rc OK, no ring slot written (published 0), the n staged events
         * become losses (older_lost +n), staging back to 0 */
        l.published = 0, l.staged = -(int64_t)n, l.lost = n;
        break;
    default:
        break; /* P-empty: every field 0 */
    }
    return l;
}

int ob_ledger_check(int c, const ob_ledger *g, char *why, unsigned long n)
{
    ob_ledger p = ob_planned(c);
#define F(f)                                                                                                         \
    if (g->f != p.f) {                                                                                                 \
        snprintf(why, n, "%s: " #f " %lld, planned %lld", ob_case_table[c].name, (long long)g->f, (long long)p.f);     \
        return -1;                                                                                                     \
    }
    F(seq) F(dropped) F(regs) F(depth) F(staged) F(sseq) F(tail_lost) F(published) F(lost)
#undef F
    if (n)
        why[0] = 0;
    return 0;
}
