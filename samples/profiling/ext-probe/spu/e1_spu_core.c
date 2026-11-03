/* E1 SPU program logic: see e1_spu_core.h.  Three descriptor records (one per textual macro use): the epoch-1 zone,
 * the epoch-2 zone and the mark.  Per epoch: 64 zones (BEGIN/END) with a MARK after zones 16, 32, 48 and 64 = 132
 * events; a flush as soon as 16 or more events are pending since the last flush (after the zone and its mark, if
 * any), and at the epoch's end. */
#include "e1_spu_core.h"

#include <string.h>

#ifndef E1_HOST_TEST
#define E1_ZONE_BEGIN(w, name) PS3DK_PROF_SPU_ZONE_BEGIN(w, name)
#define E1_MARK(w, name) PS3DK_PROF_SPU_MARK(w, name)
#else
/* HOST ONLY: the SDK record macro's ls_addr initializer (a 32-bit address) is not a constant on an LP64 host, so the
 * host build places its own 32-byte records in the same section (ls_addr 0, the real name/file); the index is the
 * same 16-byte address arithmetic, so on the host the records sit at indices 0, 2, 4 (the harness registers a filler
 * at the odd ones).  The target build uses the SDK macros above. */
struct e1_host_rec {
    struct ps3dk_prof_spu_desc d;
    uint8_t pad[32u - sizeof(struct ps3dk_prof_spu_desc)];
};
#define E1__HOST_REC(var, name)                                                                                        \
    static const struct e1_host_rec var __attribute__((section("ps3dkprof_spudesc"), aligned(16), used)) = {          \
        {0u, (uint32_t)__LINE__, (name), __FILE__}, {0}}
#define E1_ZONE_BEGIN(w, name)                                                                                         \
    __extension__({                                                                                                    \
        E1__HOST_REC(e1__rec, name);                                                                                   \
        uint32_t e1__i = PS3DK_PROF__SPU_INDEX(&e1__rec);                                                              \
        ps3dk_prof_spu_begin((w), e1__i);                                                                              \
        e1__i;                                                                                                         \
    })
#define E1_MARK(w, name)                                                                                               \
    do {                                                                                                               \
        E1__HOST_REC(e1__rec, name);                                                                                   \
        ps3dk_prof_spu_mark((w), PS3DK_PROF__SPU_INDEX(&e1__rec));                                                     \
    } while (0)
#endif

typedef struct e1_state {
    const e1_spu_port *port;
    ps3dk_prof_spu_writer *w;
    e1_line payload; /* filled, then sent; never rewritten before the port's report() returned */
    uint32_t ev[2];
    uint32_t fail_call;
    int32_t fail_rc;
    uint32_t dec_state;
    int32_t dec_rc;
    uint32_t ridx_state;
    uint64_t seen_ridx;
} e1_state;

static void fail(e1_state *s, uint32_t call, int32_t rc)
{
    if (s->fail_call == E1_CALL_NONE) { /* the FIRST error is kept */
        s->fail_call = call;
        s->fail_rc = rc ? rc : -1; /* a recorded error always has a nonzero rc */
    }
}

static void send(e1_state *s, uint32_t kind)
{
    uint32_t *p = s->payload.w;
    memset(&s->payload, 0, sizeof s->payload);
    p[E1_W_MAGIC] = E1_MAGIC;
    p[E1_W_KIND] = kind;
    p[E1_W_SEQ] = kind;
    p[E1_W_FAIL_CALL] = s->fail_call;
    p[E1_W_FAIL_RC] = (uint32_t)s->fail_rc;
    p[E1_W_EV1] = s->ev[0];
    p[E1_W_EV2] = kind == E1_KIND_FINAL ? s->ev[1] : 0u;
    p[E1_W_DEC_STATE] = s->dec_state;
    p[E1_W_DEC_RC] = (uint32_t)s->dec_rc;
    p[E1_W_RIDX_STATE] = s->ridx_state;
    if (s->ridx_state == E1_RIDX_VALID) {
        p[E1_W_RIDX_HI] = (uint32_t)(s->seen_ridx >> 32);
        p[E1_W_RIDX_LO] = (uint32_t)s->seen_ridx;
    }
    p[E1_W_DESC] = s->w->desc_count;
    s->port->report(s->port->ctx, &s->payload, kind);
}

/* an event fault already latched by begin/end/mark (an invalid descriptor index) is reported as call 5 BEFORE the
 * flush; a fault that arises inside the flush itself stays call 2 (its origin is not inferred from EFAULT) */
static int flush_checked(e1_state *s)
{
    int rc;
    if (s->w->bad_index) {
        fail(s, E1_CALL_EVENT, PS3DK_PROF_SPU_EFAULT);
        return -1;
    }
    rc = ps3dk_prof_spu_flush(s->w);
    if (rc != PS3DK_PROF_SPU_OK) {
        fail(s, E1_CALL_FLUSH, rc);
        return -1;
    }
    return 0;
}

static volatile uint32_t e1_sink; /* keeps the fixed integer work */

static void work(uint32_t z)
{
    uint32_t x = z * 2654435761u;
    for (uint32_t i = 0; i < 64u; i++)
        x = x * 1664525u + 1013904223u;
    e1_sink += x;
}

static void mark(e1_state *s, unsigned e)
{
    E1_MARK(s->w, "probe-mark");
    s->ev[e]++;
}

/* one epoch; returns 0, or -1 after recording the first error */
static int epoch(e1_state *s, unsigned e)
{
    uint32_t since = 0; /* events issued since the last flush */
    for (uint32_t z = 0; z < 64u; z++) {
        uint32_t b, before = s->ev[e];
        if (e == 0u)
            b = E1_ZONE_BEGIN(s->w, "probe-e1");
        else
            b = E1_ZONE_BEGIN(s->w, "probe-e2");
        work(z);
        PS3DK_PROF_SPU_ZONE_END(s->w, b);
        s->ev[e] += 2u;
        if ((z + 1u) % 16u == 0u)
            mark(s, e);
        since += s->ev[e] - before;
        if (since >= 16u) {
            if (flush_checked(s) != 0)
                return -1;
            since = 0;
        }
    }
    return flush_checked(s);
}

void e1_spu_run(const e1_spu_port *port, ps3dk_prof_spu_writer *w, void *staging, uint64_t ea_ring,
                uint32_t ring_slots, uint64_t ea_ctl)
{
    static e1_state st;
    e1_state *s = &st;
    uint32_t cmd;
    int rc;
    memset(s, 0, sizeof *s);
    s->port = port;
    s->w = w;
    rc = ps3dk_prof_spu_attach(w, ea_ring, ring_slots, ea_ctl, E1_WRITER_TAG, staging, E1_LS_CAP,
                               PS3DK_PROF_SPU_OWN_DEC);
    if (rc != PS3DK_PROF_SPU_OK) { /* EBUSY included: E1 enables no events, so it is a defect, never retried */
        fail(s, E1_CALL_ATTACH, rc);
        send(s, E1_KIND_EPOCH1);
        return;
    }
    if (epoch(s, 0u) != 0) {
        send(s, E1_KIND_EPOCH1);
        return;
    }
    send(s, E1_KIND_EPOCH1); /* clean: ev1 == 132 */
    cmd = port->read_in_mbox(port->ctx);
    if (cmd != E1_GO) { /* validated before any phase-2 action */
        fail(s, E1_CALL_GO, -1);
        send(s, E1_KIND_FINAL);
        return;
    }
    /* the first post-GO flush snapshots line A: its r_seen is the read_idx the PPU's drain stored */
    if (flush_checked(s) != 0) {
        send(s, E1_KIND_FINAL);
        return;
    }
    s->seen_ridx = w->s.r_seen; /* read IMMEDIATELY, before any later flush can update it */
    s->ridx_state = E1_RIDX_VALID;
    rc = ps3dk_prof_spu_dec_write(w, 0xFFFFFFFFu);
    if (rc == PS3DK_PROF_SPU_EAGAIN) { /* preflight refused BEFORE any write: one flush, one retry */
        if (flush_checked(s) != 0) {
            send(s, E1_KIND_FINAL);
            return;
        }
        rc = ps3dk_prof_spu_dec_write(w, 0xFFFFFFFFu);
    }
    if (rc != PS3DK_PROF_SPU_OK) { /* EFAULT never retried: the write may have happened */
        s->dec_state = E1_DEC_FAILED;
        s->dec_rc = rc;
        fail(s, E1_CALL_DEC, rc);
        send(s, E1_KIND_FINAL);
        return;
    }
    s->dec_state = E1_DEC_OK;
    if (epoch(s, 1u) != 0) {
        send(s, E1_KIND_FINAL);
        return;
    }
    rc = ps3dk_prof_spu_fini(w);
    if (rc != PS3DK_PROF_SPU_OK)
        fail(s, E1_CALL_FINI, rc);
    send(s, E1_KIND_FINAL);
}
