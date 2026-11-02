/* Host model infrastructure for the SPU lane transport (see model.h).  Model evidence only. */
#include "model.h"

#include <stdlib.h>
#include <string.h>

#define MEM_BYTES (40u << 20)

uint8_t *g_mem;
uint64_t g_tb;
int g_weak;
int g_failures;

/* ---- allocator over the simulated memory: first fit among freed blocks of the same size, else bump ---- */
typedef struct blk {
    uint64_t ea;
    size_t size;
    int used;
} blk;
static blk g_blk[4096];
static int g_nblk;
static uint64_t g_top;

/* weak visibility queue: each putlluc is 3 groups {gen}, {fields}, {gen_end}; groups apply in any order */
typedef struct wgroup {
    uint64_t ea;
    uint32_t off, len;
    uint8_t bytes[128];
} wgroup;
static wgroup g_wq[64];
static int g_nwq;

static uint8_t (*g_ver)[128];
static size_t g_nver, g_capver;

void versions_reset(void) { g_nver = 0; }

int versions_has(const uint8_t line[128])
{
    static const uint8_t zero[128];
    size_t i;
    if (!memcmp(line, zero, 128))
        return 1;
    for (i = 0; i < g_nver; i++)
        if (!memcmp(g_ver[i], line, 128))
            return 1;
    return 0;
}

static void version_add(const uint8_t *line)
{
    if (g_nver == g_capver) {
        g_capver = g_capver ? g_capver * 2 : 64;
        g_ver = realloc(g_ver, sizeof *g_ver * g_capver);
    }
    memcpy(g_ver[g_nver++], line, 128);
}

void mem_reset(void)
{
    if (!g_mem) {
        g_mem = malloc(MEM_BYTES);
        memset(g_mem, 0, MEM_BYTES);
    } else {
        memset(g_mem, 0, g_top < MEM_BYTES ? g_top : MEM_BYTES); /* only what the last run touched */
    }
    versions_reset();
    g_nblk = 0;
    g_top = 4096; /* EA 0 is never handed out */
    g_nwq = 0;
    g_tb = 1000;
}

static int m_alloc(void *ctx, size_t size, uint64_t *ea)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_nblk; i++)
        if (!g_blk[i].used && g_blk[i].size == size) {
            g_blk[i].used = 1;
            memset(g_mem + g_blk[i].ea, 0, size);
            *ea = g_blk[i].ea;
            return 0;
        }
    if (g_nblk == 4096 || g_top + size > MEM_BYTES)
        return -1;
    g_blk[g_nblk].ea = g_top;
    g_blk[g_nblk].size = size;
    g_blk[g_nblk].used = 1;
    g_nblk++;
    *ea = g_top;
    memset(g_mem + g_top, 0, size);
    g_top += (size + 127) & ~(uint64_t)127;
    return 0;
}

static void m_free(void *ctx, uint64_t ea, size_t size)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_nblk; i++)
        if (g_blk[i].ea == ea) {
            if (!g_blk[i].used || g_blk[i].size != size)
                FAIL("bad free of %llu", (unsigned long long)ea);
            g_blk[i].used = 0;
            memset(g_mem + ea, 0xDD, size); /* a later access by anyone would read poison */
            return;
        }
    FAIL("free of an unknown block %llu", (unsigned long long)ea);
}

int mem_pending(void) { return g_nwq; }

void mem_apply(int which)
{
    if (which >= g_nwq)
        return;
    memcpy(g_mem + g_wq[which].ea + g_wq[which].off, g_wq[which].bytes, g_wq[which].len);
    memmove(&g_wq[which], &g_wq[which + 1], sizeof g_wq[0] * (size_t)(g_nwq - which - 1));
    g_nwq--;
}

void mem_apply_all(void)
{
    while (g_nwq)
        mem_apply(0);
}

/* ---- PPU memory shim, with two model checks:
 *  - no PPU access to a freed block (the drainer's release must be its last access);
 *  - ORDER: after a load of a line-B generation word, no ring load may happen before an lwsync (or sync).  The shim
 *    has no reordering; this checks the program order the target barrier relies on, not target visibility. */
int g_need_bar;
int g_order_faults;

static void access_check(uint64_t ea, size_t n, const char *what)
{
    int i;
    for (i = 0; i < g_nblk; i++)
        if (ea >= g_blk[i].ea && ea + n <= g_blk[i].ea + g_blk[i].size) {
            if (!g_blk[i].used)
                FAIL("PPU %s of freed memory at %llu", what, (unsigned long long)ea);
            return;
        }
    FAIL("PPU %s outside any block at %llu", what, (unsigned long long)ea);
}

static int is_gen_word(uint64_t ea)
{
    int i;
    for (i = 0; i < g_nblk; i++)
        if (g_blk[i].size == PSP_CTL_BYTES && ea == g_blk[i].ea + PSP_B_GEN)
            return 1;
    return 0;
}

static uint64_t m_ld64(void *c, uint64_t ea)
{
    (void)c;
    access_check(ea, 8, "load");
    if (is_gen_word(ea))
        g_need_bar = 1;
    return psp_rd64(g_mem + ea);
}
static void m_ld_line(void *c, uint64_t ea, uint8_t out[128])
{
    (void)c;
    access_check(ea, 128, "line load");
    memcpy(out, g_mem + ea, 128);
}
static void m_ld_slots(void *c, uint64_t ea, uint8_t *out, size_t n)
{
    (void)c;
    access_check(ea, n, "ring load");
    if (g_need_bar) {
        FAIL("ring load after a generation load with no barrier between them");
        g_order_faults++;
    }
    memcpy(out, g_mem + ea, n);
}
static void m_st64(void *c, uint64_t ea, uint64_t v)
{
    (void)c;
    access_check(ea, 8, "store");
    psp_be64(g_mem + ea, v);
}
static void m_st16(void *c, uint64_t ea, uint16_t v)
{
    (void)c;
    access_check(ea, 2, "store");
    psp_be16(g_mem + ea, v);
}
static void m_bar(void *c)
{
    (void)c;
    g_need_bar = 0; /* lwsync / sync */
}
static void m_isync(void *c) { (void)c; }
static uint64_t m_mftb(void *c) { (void)c; return g_tb += 7; }

/* ---- SPU port shim ---- */
static int injected(port_ctx *pc)
{
    pc->ops++;
    return pc->fault_at_op && pc->ops == pc->fault_at_op;
}
static uint32_t p_rd_dec(void *c)
{
    port_ctx *pc = c;
    pc->dec -= 3;
    pc->last_dec = pc->dec;
    return pc->dec;
}
static void p_wr_dec(void *c, uint32_t v)
{
    port_ctx *pc = c;
    pc->dec = v;
    if (pc->nwr < 8)
        pc->wr_log[pc->nwr] = v;
    pc->nwr++;
}
static int p_blocking(void *c) { return ((port_ctx *)c)->blocking; }
static uint32_t p_rd_mask(void *c) { return ((port_ctx *)c)->tag_mask; }
static void p_wr_mask(void *c, uint32_t m) { ((port_ctx *)c)->tag_mask = m; }
static uint32_t p_tag_wait(void *c)
{
    port_ctx *pc = c;
    return injected(pc) ? 0 : pc->tag_mask;
}
static void p_getllar(void *c, uint64_t ea, void *ls)
{
    port_ctx *pc = c;
    memcpy(ls, g_mem + ea, 128);
    pc->res_held = 1;
    pc->last_stat = injected(pc) ? 0 : PSP_AS_GETLLAR;
}
static void p_putllc(void *c, uint64_t ea, const void *ls)
{
    port_ctx *pc = c;
    int same = memcmp(ls, g_mem + ea, 128) == 0;
    if (!pc->res_held)
        FAIL("putllc without a reservation");
    pc->res_held = 0; /* success or loss, the reservation ends */
    pc->last_stat = injected(pc) ? 0 : (same ? PSP_AS_PUTLLC_OK : PSP_AS_PUTLLC_LOST);
}
static void p_putlluc(void *c, uint64_t ea, const void *ls)
{
    port_ctx *pc = c;
    if (injected(pc) ||
        (pc->fail_closed_store && psp_rd32((const uint8_t *)ls + PSP_B_STATE - PSP_B) == PSP_ST_CLOSED)) {
        pc->last_stat = 0; /* the store never completes: nothing becomes visible */
        return;
    }
    version_add(ls);
    if (g_weak) { /* independently visible groups: {gen} {fields} {gen_end} */
        static const uint32_t off[3] = {0, 8, 120}, len[3] = {8, 112, 8};
        int i;
        for (i = 0; i < 3; i++) {
            wgroup *g = &g_wq[g_nwq++];
            g->ea = ea;
            g->off = off[i];
            g->len = len[i];
            memcpy(g->bytes, (const uint8_t *)ls + off[i], len[i]);
        }
    } else {
        memcpy(g_mem + ea, ls, 128);
    }
    pc->last_stat = PSP_AS_PUTLLUC;
}
static uint32_t p_atomic_stat(void *c)
{
    port_ctx *pc = c;
    uint32_t s = pc->last_stat;
    pc->last_stat = 0;
    return s;
}
static void p_put(void *c, const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    port_ctx *pc = c;
    psp_spu *s = pc->s;
    uint64_t r_mem = psp_rd64(g_mem + s->ea_ctl + PSP_A_READ_IDX);
    (void)tag;
    if (!((size == 8 && !(ea & 7) && !((uintptr_t)ls & 7)) ||
          (size && !(size & 15) && size <= 16384 && !(ea & 15) && !((uintptr_t)ls & 15))))
        FAIL("invalid DMA size %u ea %llu", size, (unsigned long long)ea);
    if (ea < s->ea_ring || ea + size > s->ea_ring + (uint64_t)s->ring_slots * PSP_SLOT)
        FAIL("DMA outside the ring");
    if (s->w + s->pub_k - r_mem > s->ring_slots)
        FAIL("producer would overwrite unread data: w %llu k %u r %llu", (unsigned long long)s->w, s->pub_k,
             (unsigned long long)r_mem);
    memcpy(g_mem + ea, ls, size);
    pc->dma_count++;
}

static int o_chunk(void *ctx, uint32_t type, const uint8_t *payload, size_t len)
{
    run *r = ctx;
    if (r->sink_dead)
        return -1; /* a sink failure is terminal */
    if ((r->fail_sink_at >= 0 && (long)r->nch >= r->fail_sink_at) ||
        (r->fail_type && type == r->fail_type && ++r->seen_type == r->fail_nth)) {
        r->sink_dead = 1;
        return -1;
    }
    if (r->nch == r->capch) {
        r->capch = r->capch ? r->capch * 2 : 256;
        r->ch = realloc(r->ch, sizeof *r->ch * r->capch);
    }
    r->ch[r->nch].type = type;
    r->ch[r->nch].len = (uint32_t)len;
    r->ch[r->nch].data = malloc(len ? len : 1);
    memcpy(r->ch[r->nch].data, payload, len);
    r->nch++;
    return 0;
}

static const psp_desc g_descs[3] = {{0x100, 10, "spu_a", "spu/a.c"}, {0x110, 11, "spu_b", "spu/a.c"},
                                    {0x120, 12, "spu_c", "spu/b.c"}};

void run_init(run *r, uint32_t ring_slots, uint32_t lanes_max, const psp_limits *lim)
{
    run_init_cap(r, ring_slots, lanes_max, 64ull << 20, lim);
}

void run_init_cap(run *r, uint32_t ring_slots, uint32_t lanes_max, uint64_t mem_max, const psp_limits *lim)
{
    psp_cfg cfg = {ring_slots, lanes_max, mem_max, 1024};
    psp_limits rl = psp_reader_limits();
    memset(r, 0, sizeof *r);
    mem_reset();
    r->fail_sink_at = -1;
    r->m = (psp_mem){NULL, m_ld64, m_ld_line, m_ld_slots, m_st64, m_st16, m_bar, m_bar, m_isync, m_mftb, m_alloc, m_free};
    g_need_bar = 0;
    r->o = (psp_out){r, o_chunk};
    if (psp_ppu_init(&r->p, &cfg, lim ? lim : &rl, &r->m, &r->o) != 0)
        FAIL("ppu init");
    if (psp_image_add(&r->p, (const uint8_t *)"\x02model-image", 12, g_descs, 3, &r->image) != 0)
        FAIL("image add");
}

void run_free(run *r)
{
    size_t i;
    uint32_t j;
    for (i = 0; i < r->nch; i++)
        free(r->ch[i].data);
    free(r->ch);
    for (j = 0; j < r->nl; j++) {
        free(r->lanes[j]->buf);
        free(r->lanes[j]->truth);
        free(r->lanes[j]);
    }
    psp_ppu_fini(&r->p);
}

lane_h *lane_new(run *r, uint32_t ls_cap, int own_dec, int start)
{
    lane_h *h = calloc(1, sizeof *h);
    psp_lane *pl;
    int rc;
    if (psp_lane_create(&r->p, "worker", r->image, 0, &h->idx) != 0) {
        free(h);
        return NULL;
    }
    pl = &r->p.lanes[h->idx];
    h->lane_id = pl->lane_id;
    h->pc.dec = 0xFFFFFF00u;
    h->pc.tag_mask = 0xA5u; /* the app's mask: every library wait must restore it */
    h->pc.s = &h->s;
    h->port = (psp_port){&h->pc, p_rd_dec, p_wr_dec, p_blocking, p_rd_mask, p_wr_mask, p_tag_wait, p_getllar,
                         p_putllc, p_putlluc, p_atomic_stat, p_put};
    h->buf = aligned_alloc(16, ((size_t)ls_cap + 2 * PSP_HEAD) * PSP_SLOT);
    r->lanes[r->nl++] = h;
    if (start) {
        rc = psp_spu_init(&h->s, &h->port, pl->ea_ring, r->p.cfg.ring_slots, pl->ea_ctl, 5, h->buf, ls_cap, own_dec);
        if (rc != PSP_OK)
            FAIL("spu init rc %d", rc);
        call_ok(h, "init");
    }
    return h;
}

void lane_drop(run *r, lane_h *h)
{
    (void)r;
    free(h->buf);
    h->buf = NULL;
}

void inject_older(lane_h *h, uint64_t k)
{
    if (h->ntruth)
        FAIL("inject_older after real events: use it only on a fresh lane");
    h->s.older_lost += k;
    h->s.seq += k;
    h->tbase += k;
}

void call_ok(lane_h *h, const char *what)
{
    if (h->pc.res_held)
        FAIL("%s: a reservation outlived the call", what);
    if (h->pc.tag_mask != 0xA5u)
        FAIL("%s: tag mask not restored (%#x)", what, h->pc.tag_mask);
}

static void truth_add(lane_h *h, uint8_t kind, uint16_t desc, uint8_t depth)
{
    if (h->ntruth == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->truth = realloc(h->truth, sizeof *h->truth * h->cap);
    }
    h->truth[h->ntruth].dec = h->pc.last_dec;
    h->truth[h->ntruth].desc = desc;
    h->truth[h->ntruth].kind = kind;
    h->truth[h->ntruth].depth = depth;
    h->ntruth++;
}

/* the truth is taken from the producer's own decision: an event that took a sequence number */
void zb(lane_h *h, uint16_t d)
{
    uint64_t q = h->s.seq;
    psp_spu_begin(&h->s, d);
    if (h->s.seq != q) /* a dropped event's content is never compared: it is lost */
        truth_add(h, PSP_K_BEGIN, d, (uint8_t)(h->s.depth - 1));
}
void ze(lane_h *h, uint16_t d)
{
    uint64_t q = h->s.seq;
    psp_spu_end(&h->s, d);
    if (h->s.seq != q)
        truth_add(h, PSP_K_END, d, (uint8_t)h->s.depth);
}
void zm(lane_h *h, uint16_t d)
{
    uint64_t q = h->s.seq;
    psp_spu_mark(&h->s, d);
    if (h->s.seq != q)
        truth_add(h, PSP_K_MARK, d, (uint8_t)h->s.depth);
}

/* ---- the v1 skeleton ---- */
static uint32_t crc_tab[256];
static uint32_t crc32b(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!crc_tab[1])
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t x = k;
            for (int j = 0; j < 8; j++)
                x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            crc_tab[k] = x;
        }
    for (i = 0; i < n; i++)
        c = crc_tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct wbuf {
    uint8_t *b;
    size_t n, cap;
} wbuf;
static void wb(wbuf *w, const void *p, size_t n)
{
    if (w->n + n > w->cap) {
        w->cap = (w->n + n) * 2;
        w->b = realloc(w->b, w->cap);
    }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void wle(wbuf *w, uint64_t v, int bytes)
{
    uint8_t t[8];
    for (int i = 0; i < bytes; i++)
        t[i] = (uint8_t)(v >> (8 * i));
    wb(w, t, (size_t)bytes);
}
static void wstr(wbuf *w, const char *s)
{
    wle(w, strlen(s), 2);
    wb(w, s, strlen(s));
}

static void frame(wbuf *f, uint64_t *seq, uint32_t type, const uint8_t *payload, size_t len)
{
    size_t start = f->n;
    wle(f, type, 4);
    wle(f, len, 4);
    wle(f, (*seq)++, 8);
    wb(f, payload, len);
    wle(f, crc32b(f->b + start, f->n - start), 4);
}

void write_capture(run *r, const char *dir, const char *name)
{
    wbuf f = {0}, p = {0};
    uint64_t seq = 0;
    size_t i;
    char path[512];
    FILE *fp;
    uint32_t j;
    wb(&f, "PS3DKPRF", 8);
    wle(&f, 1, 2);
    wle(&f, 0, 2);
    wle(&f, 1, 4); /* file_flags: DOMAIN_EXTENSIONS */
    wstr(&p, "app");
    wstr(&p, "test-build-0001");
    wstr(&p, "0.26.0");
    wstr(&p, "gcc 16.1.0");
    wstr(&p, "-O2");
    wstr(&p, "spu-model");
    wle(&p, 1, 1);
    wle(&p, 1, 1);
    wle(&p, 0, 2);
    wle(&p, 7, 8);
    wle(&p, 4096, 4);
    wle(&p, 8, 4);
    wle(&p, 16, 4);
    frame(&f, &seq, 1, p.b, p.n);
    p.n = 0;
    wle(&p, 1, 4);
    wle(&p, 1, 4);
    wle(&p, 0, 4);
    wle(&p, 79800000, 8);
    wle(&p, 1000, 8);
    frame(&f, &seq, 2, p.b, p.n);
    for (i = 0; i < r->nch; i++)
        frame(&f, &seq, r->ch[i].type, r->ch[i].data, r->ch[i].len);
    if (!r->p.sink_failed) {
        p.n = 0;
        wle(&p, 0, 4);
        wle(&p, 0, 4);
        wle(&p, 0, 8);
        wle(&p, 0, 4);
        wle(&p, 0, 4);
        wle(&p, 0, 8);
        wle(&p, 0, 8);
        frame(&f, &seq, 6, p.b, p.n);
    }
    snprintf(path, sizeof path, "%s/%s.prof", dir, name);
    fp = fopen(path, "wb");
    if (!fp || fwrite(f.b, 1, f.n, fp) != f.n || fclose(fp) != 0)
        FAIL("write %s", path);
    snprintf(path, sizeof path, "%s/%s.truth", dir, name);
    fp = fopen(path, "w");
    for (j = 0; fp && j < r->nl; j++) {
        lane_h *h = r->lanes[j];
        /* the coverage contract: seqs below base were injected as lost; [base, base+export) are listed;
         * [base+export, base+count) exist but are not exported (the full C stream check covers them) */
        fprintf(fp, "#lane %u base %llu count %llu export %llu\n", h->lane_id, (unsigned long long)h->tbase,
                (unsigned long long)h->ntruth, (unsigned long long)(h->ntruth < 4096 ? h->ntruth : 4096));
        for (uint64_t q = 0; q < h->ntruth && q < 4096; q++)
            fprintf(fp, "%u %llu %u %u %u %u\n", h->lane_id, (unsigned long long)(h->tbase + q), h->truth[q].dec, h->truth[q].desc,
                    h->truth[q].kind, h->truth[q].depth);
    }
    if (fp)
        fclose(fp);
    free(f.b);
    free(p.b);
}

static uint32_t rle32x(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rle64x(const uint8_t *p) { return rle32x(p) | (uint64_t)rle32x(p + 4) << 32; }

void accepted_counts(run *r, uint32_t lane_id, uint64_t *next, uint64_t *events, uint64_t *lost, int *final_present)
{
    size_t i;
    *next = *events = *lost = 0;
    *final_present = 0;
    for (i = 0; i < r->nch; i++) {
        const uint8_t *d = r->ch[i].data;
        if (r->ch[i].type == 13 && rle32x(d) == lane_id) {
            *events += rle32x(d + 16);
        } else if (r->ch[i].type == 15 && rle32x(d) == lane_id && d[4] == 1) {
            *lost += rle64x(d + 16);
        } else if (r->ch[i].type == 16) {
            for (uint32_t e = 0; e < rle32x(d); e++)
                if (rle32x(d + 4 + 36 * e) == lane_id && d[4 + 36 * e + 4] == 1)
                    *final_present = 1;
        }
    }
    *next = *events + *lost;
}

void check_stats(run *r, lane_h *h, const char *name)
{
    psp_lane_stats st;
    uint64_t next, events, lost;
    int fin;
    if (psp_lane_stats_get(&r->p, h->idx, 0, &st) != PSP_P_OK) {
        FAIL("%s: no stats (the lane was freed)", name);
        return;
    }
    accepted_counts(r, h->lane_id, &next, &events, &lost, &fin);
    if (st.next_seq != next || st.events != events || st.lost != lost || st.final_written != fin)
        FAIL("%s lane %u: stats %llu/%llu/%llu final_written %d, accepted %llu/%llu/%llu FINAL %d", name, h->lane_id,
             (unsigned long long)st.next_seq, (unsigned long long)st.events, (unsigned long long)st.lost,
             st.final_written, (unsigned long long)next, (unsigned long long)events, (unsigned long long)lost, fin);
}

FILE *expect_open(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.expect", dir, name);
    return fopen(path, "w");
}

/* ---- the independent stream check ---- */
static uint32_t rle32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rle64(const uint8_t *p) { return rle32(p) | (uint64_t)rle32(p + 4) << 32; }

int check_streams(run *r, const char *name)
{
    uint32_t j;
    int bad = 0;
    for (j = 0; j < r->nl; j++) {
        lane_h *h = r->lanes[j];
        uint64_t next = 0, events = 0, lost = 0;
        int fin = 0;
        size_t i;
        for (i = 0; i < r->nch; i++) {
            const uint8_t *d = r->ch[i].data;
            if (r->ch[i].type == 13 && rle32(d) == h->lane_id) {
                uint64_t first = rle64(d + 8), n = rle32(d + 16), k;
                if (fin || first != next) {
                    FAIL("%s lane %u: SPU_EVENTS first %llu, want %llu", name, h->lane_id, (unsigned long long)first,
                         (unsigned long long)next);
                    bad = 1;
                }
                for (k = 0; k < n; k++) {
                    const uint8_t *e = d + 20 + 8 * k;
                    uint64_t q = first + k, t = q - h->tbase;
                    if (q < h->tbase || t >= h->ntruth || rle32(e) != h->truth[t].dec ||
                        (uint16_t)(e[4] | e[5] << 8) != h->truth[t].desc || e[6] != h->truth[t].kind ||
                        e[7] != h->truth[t].depth) {
                        FAIL("%s lane %u: event seq %llu does not match the producer's event", name, h->lane_id,
                             (unsigned long long)q);
                        bad = 1;
                        break;
                    }
                }
                next += n;
                events += n;
            } else if (r->ch[i].type == 15 && rle32(d) == h->lane_id && d[4] == 1) {
                uint64_t first = rle64(d + 8), n = rle64(d + 16);
                if (fin || first != next || first + n > h->tbase + h->ntruth) {
                    FAIL("%s lane %u: LOSS %llu+%llu, want first %llu within %llu", name, h->lane_id,
                         (unsigned long long)first, (unsigned long long)n, (unsigned long long)next,
                         (unsigned long long)h->ntruth);
                    bad = 1;
                }
                next += n;
                lost += n;
            } else if (r->ch[i].type == 16) {
                uint32_t cnt = rle32(d), e;
                for (e = 0; e < cnt; e++) {
                    const uint8_t *f = d + 4 + 36 * e;
                    if (rle32(f) != h->lane_id || f[4] != 1)
                        continue;
                    fin++;
                    if (rle64(f + 8) != next || rle64(f + 16) != events || rle64(f + 24) != lost) {
                        FAIL("%s lane %u: FINAL %llu/%llu/%llu, emitted %llu/%llu/%llu", name, h->lane_id,
                             (unsigned long long)rle64(f + 8), (unsigned long long)rle64(f + 16),
                             (unsigned long long)rle64(f + 24), (unsigned long long)next, (unsigned long long)events,
                             (unsigned long long)lost);
                        bad = 1;
                    }
                }
            }
        }
        if (fin > 1) {
            FAIL("%s lane %u: %d FINAL entries", name, h->lane_id, fin);
            bad = 1;
        }
    }
    return bad;
}
