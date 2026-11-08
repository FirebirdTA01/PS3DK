/* Host-model checks of the SPU_ORDER records (spu-ticks slice design 3, delta D3): producer ground truth of every
 * published CAL slot, the order ledger invariant, and an independent check of the accepted records rebuilt from the
 * accepted chunk bytes only (never from the drain's state).  Model evidence only. */
#include <stdlib.h>
#include <string.h>

#include "model.h"

static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }

void cal_note(lane_h *h, uint16_t answered_before)
{
    if (h->s.cal_answered == answered_before)
        return; /* no CAL was published by this call */
    if (h->ncal == h->capcal) {
        h->capcal = h->capcal ? h->capcal * 2 : 16;
        h->cal = realloc(h->cal, sizeof *h->cal * h->capcal);
    }
    /* the CAL slot's raw value is the plan's last decrementer read; every seq below s.seq was published before it */
    h->cal[h->ncal][0] = h->pc.last_dec;
    h->cal[h->ncal][1] = h->s.seq;
    h->ncal++;
}

void order_ledger_ok(run *r, const char *where)
{
    if (r->p.order_emitted + r->p.order_reserved > r->p.lim.order_records)
        FAIL("%s: order ledger %llu emitted + %llu reserved > %u", where, (unsigned long long)r->p.order_emitted,
             (unsigned long long)r->p.order_reserved, r->p.lim.order_records);
}

static int lane_ix(run *r, uint32_t lane_id)
{
    for (uint32_t j = 0; j < r->nl; j++)
        if (r->lanes[j]->lane_id == lane_id)
            return (int)j;
    return -1;
}

static int lane_of_domain(run *r, uint32_t domain)
{
    for (uint32_t j = 0; j < r->nl; j++)
        if (r->lanes[j]->domain_id == domain)
            return (int)j;
    return -1;
}

uint64_t order_count(run *r, uint32_t lane_id, uint32_t kind)
{
    uint64_t c = 0;
    for (size_t i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        if (r->ch[i].type != 18)
            continue;
        for (uint32_t k = 0; k < r32(d) && 8 + 24 * (k + 1) <= r->ch[i].len; k++)
            if (r32(d + 8 + 24 * k) == kind && r32(d + 12 + 24 * k) == lane_id)
                c++;
    }
    return c;
}

int order_final(run *r, uint32_t lane_id, uint64_t *emitted, uint64_t *dropped)
{
    *emitted = *dropped = 0;
    for (size_t i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        if (r->ch[i].type == 18 && r32(d) == 1 && r32(d + 8) == 5 && r32(d + 12) == lane_id) {
            *emitted = r64(d + 16);
            *dropped = r64(d + 24);
            return 1;
        }
    }
    return 0;
}

typedef struct ost {
    int declared;
    uint32_t created, records, finals, epoch;
    uint64_t cursor, prev_through, last_tb;
    int have_tb;
    uint32_t *evep; /* per seq: the event's epoch, 0 = lost */
    uint64_t evcap;
} ost;

typedef struct obr {
    int lane;
    uint32_t epoch;
    uint64_t seq, foreign;
} obr;

static void evep_set(ost *o, uint64_t seq, uint32_t e)
{
    if (seq >= o->evcap) {
        uint64_t n = o->evcap ? o->evcap : 64;
        while (n <= seq)
            n *= 2;
        o->evep = realloc(o->evep, sizeof *o->evep * n);
        memset(o->evep + o->evcap, 0, sizeof *o->evep * (n - o->evcap));
        o->evcap = n;
    }
    o->evep[seq] = e;
}

unsigned check_order(run *r, const char *name, int flags)
{
    unsigned bad = 0;
    ost *o = calloc(r->nl ? r->nl : 1, sizeof *o);
    obr *br = NULL;
    size_t nbr = 0, capbr = 0;
#define V(cls, ...)                                                                                                    \
    do {                                                                                                               \
        if (!(bad & (cls)))                                                                                            \
            fprintf(stderr, "order check %s: ", name), fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n");          \
        bad |= (cls);                                                                                                  \
    } while (0)
    for (size_t i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        uint32_t t = r->ch[i].type, len = r->ch[i].len;
        if (t == 12) { /* SPU_LANES */
            size_t at = 4;
            for (uint32_t e = 0; e < r32(d); e++) {
                int j = lane_ix(r, r32(d + at));
                if (j >= 0)
                    o[j].declared = 1;
                at += 24;
                at += 2 + (size_t)(d[at] | d[at + 1] << 8);
            }
        } else if (t == 9) {
            for (uint32_t e = 0; e < r32(d); e++) {
                int j = lane_of_domain(r, r32(d + 4 + 32 * e));
                if (j >= 0)
                    o[j].epoch = r32(d + 4 + 32 * e + 4);
            }
        } else if (t == 13) {
            int j = lane_ix(r, r32(d));
            if (j >= 0) {
                uint64_t first = r64(d + 8), n = r32(d + 16);
                for (uint64_t k = 0; k < n; k++)
                    evep_set(&o[j], first + k, r32(d + 4));
                o[j].cursor = first + n;
            }
        } else if (t == 15 && d[4] == 1) {
            int j = lane_ix(r, r32(d));
            if (j >= 0)
                o[j].cursor = r64(d + 8) + r64(d + 16);
        } else if (t == 10) {
            int j = lane_of_domain(r, r32(d));
            for (uint32_t k = 0; j >= 0 && k < r32(d + 16); k++) {
                if (nbr == capbr) {
                    capbr = capbr ? capbr * 2 : 32;
                    br = realloc(br, sizeof *br * capbr);
                }
                br[nbr++] = (obr){j, r32(d + 4), r64(d + 8) + k, r64(d + 20 + 24 * (size_t)k)};
            }
        } else if (t == 16 && r->order) {
            for (uint32_t e = 0; e < r32(d); e++) {
                const uint8_t *f = d + 4 + 36 * e;
                const uint8_t *q = i ? r->ch[i - 1].data : NULL;
                if (f[4] != 1 || lane_ix(r, r32(f)) < 0)
                    continue;
                if (!q || r->ch[i - 1].type != 18 || r32(q) != 1 || r32(q + 8) != 5 || r32(q + 12) != r32(f))
                    V(OC_FINAL, "lane %u: DOMAIN_FINAL not directly after its ORDER_FINAL", r32(f));
            }
        } else if (t == 18) {
            uint32_t n = r32(d);
            if (r32(d + 4) != 0 || (n != 1 && n != 2) || len != 8 + 24 * n) {
                V(OC_LAYOUT, "chunk %zu: SPU_ORDER n %u len %u", i, n, len);
                continue;
            }
            const uint8_t *a = d + 8, *b = d + 32;
            uint32_t kind = r32(a), lane = r32(a + 4);
            int j = lane_ix(r, lane);
            if (n == 2 && (kind != 2 || r32(b) != 3 || r32(b + 4) != lane || r64(a + 16) != r64(b + 16))) {
                V(OC_LAYOUT, "chunk %zu: n 2 is not one DRAINED + DRAIN_TB pair", i);
                continue;
            }
            if (n == 1 && (kind == 2 || kind == 3)) {
                V(OC_LONE, "chunk %zu: lane %u kind %u alone", i, lane, kind);
                if (j >= 0)
                    o[j].records++; /* still an accepted record of the lane (ORDER_FINAL counts it) */
                continue;
            }
            if (n == 1 && kind != 1 && kind != 4 && kind != 5) {
                V(OC_LAYOUT, "chunk %zu: kind %u", i, kind);
                continue;
            }
            if (j < 0 || !o[j].declared) {
                V(kind == 4 ? OC_CREATED : OC_LAYOUT, "chunk %zu: lane %u is not declared", i, lane);
                continue;
            }
            ost *s = &o[j];
            lane_h *h = r->lanes[j];
            if (s->finals)
                V(OC_FINAL, "lane %u: an order record after its ORDER_FINAL", lane);
            if (kind == 4) {
                if (s->created || s->records)
                    V(OC_CREATED, "lane %u: LANE_CREATED duplicated or not first", lane);
                s->created++;
                s->records++;
            } else if (kind == 2) {
                uint32_t e = (uint32_t)r64(a + 8);
                uint64_t through = r64(a + 16), tb = r64(b + 8);
                if (e == 0 || e != s->epoch)
                    V(OC_EPOCH, "lane %u: DRAINED epoch %u, the current epoch is %u", lane, e, s->epoch);
                if (through != s->cursor)
                    V(OC_CURSOR, "lane %u: DRAINED events_through %llu, the cursor is %llu", lane,
                      (unsigned long long)through, (unsigned long long)s->cursor);
                if (flags & CK_RUN)
                    for (uint64_t q = s->prev_through; q < through && q < s->evcap; q++)
                        if (s->evep[q] && s->evep[q] != e) {
                            V(OC_RUN, "lane %u: DRAINED epoch %u covers seq %llu of epoch %u", lane, e,
                              (unsigned long long)q, s->evep[q]);
                            break;
                        }
                if (!tb_after_ring_load(tb, h->ea_ring, h->ring_bytes))
                    V(OC_TBORDER, "lane %u: DRAIN_TB tb %llu was not read right after a ring load of the lane", lane,
                      (unsigned long long)tb);
                if (s->have_tb && tb < s->last_tb)
                    V(OC_TBORDER, "lane %u: tb_copy decreases", lane);
                s->have_tb = 1;
                s->last_tb = tb;
                s->prev_through = through;
                s->records += 2;
            } else if (kind == 1) {
                uint32_t e = (uint32_t)r64(a + 8);
                uint64_t bseq = r64(a + 8) >> 32, before = r64(a + 16), want = UINT64_MAX;
                size_t k;
                for (k = 0; k < nbr; k++)
                    if (br[k].lane == j && br[k].epoch == e && br[k].seq == bseq)
                        break;
                if (k < nbr)
                    for (size_t c = 0; c < h->ncal; c++)
                        if (h->cal[c][0] == br[k].foreign)
                            want = h->cal[c][1];
                if (before != want)
                    V(OC_BEFORE, "lane %u epoch %u bracket %llu: events_before %llu, the producer's seq %llu", lane, e,
                      (unsigned long long)bseq, (unsigned long long)before, (unsigned long long)want);
                s->records++;
            } else { /* ORDER_FINAL */
                if (s->finals || r64(a + 8) != s->records)
                    V(OC_FINAL, "lane %u: ORDER_FINAL records_emitted %llu, accepted %u (finals %u)", lane,
                      (unsigned long long)r64(a + 8), s->records, s->finals);
                s->finals++;
            }
        }
    }
    for (uint32_t j = 0; j < r->nl; j++) {
        if (r->order && o[j].declared && o[j].created != 1 && !r->p.sink_failed)
            V(OC_CREATED, "lane %u: %u LANE_CREATED records", r->lanes[j]->lane_id, o[j].created);
        if (!o[j].declared && o[j].created)
            V(OC_CREATED, "lane %u: LANE_CREATED for an undeclared lane", r->lanes[j]->lane_id);
        free(o[j].evep);
    }
#undef V
    free(o);
    free(br);
    return bad;
}
