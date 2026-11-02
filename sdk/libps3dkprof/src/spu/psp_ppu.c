/* PPU side of the SPU lane transport (design p3.2 r3, slice 1 r2).
 *  - Counts, the replay and every ledger entry advance only for a chunk the sink ACCEPTED (emit() returned 1); a
 *    FINAL is "written" only when accepted, while retirement from encoding (PSP_L_FINAL) and release are separate.
 *  - After a sink failure nothing more is encoded; lifetime still progresses.
 *  - Every aggregate is charged to the ledger (emitted + reserved) before the work that needs it.
 *  - Published corruption fails the sink (v1 rule, code -2).
 *  - All drainer work on a lane happens inside the abstract lane-table lock (p->cs); a lane is freed only outside
 *    it, after the drainer's release, which is its last access to the lane's memory (r3 C, E). */
#include "psp_ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_DOMAIN 8u
#define C_EPOCH 9u
#define C_CALIBRATION 10u
#define C_SPU_IMAGES 11u
#define C_SPU_LANES 12u
#define C_SPU_EVENTS 13u
#define C_LOSS 15u
#define C_DOMAIN_FINAL 16u
#define MAX_CHUNK 1048576u
#define MAX_SPU_EVENTS 131069u
#define SNAP_TRIES 8u

psp_limits psp_reader_limits(void)
{
    psp_limits l = {1024, 64, 32, 4096, 65536, 65536, 262144};
    return l;
}

/* ---- chunk payloads (little-endian, the file's order) ---- */

static void le8(psp_ppu *p, uint8_t v) { p->scratch[p->spos++] = v; }
static void le16(psp_ppu *p, uint16_t v) { le8(p, (uint8_t)v); le8(p, (uint8_t)(v >> 8)); }
static void le32(psp_ppu *p, uint32_t v) { le16(p, (uint16_t)v); le16(p, (uint16_t)(v >> 16)); }
static void le64(psp_ppu *p, uint64_t v) { le32(p, (uint32_t)v); le32(p, (uint32_t)(v >> 32)); }
static void lestr(psp_ppu *p, const void *s, size_t n)
{
    le16(p, (uint16_t)n);
    memcpy(p->scratch + p->spos, s, n);
    p->spos += n;
}
static void poke32(psp_ppu *p, size_t at, uint32_t v)
{
    size_t keep = p->spos;
    p->spos = at;
    le32(p, v);
    p->spos = keep;
}

/* 1 when the sink accepted the chunk; the payload bytes stay in scratch until the next append */
static int emit(psp_ppu *p, uint32_t type)
{
    int ok = 0;
    if (!p->sink_failed) {
        if (p->out->chunk(p->out->ctx, type, p->scratch, p->spos) == 0)
            ok = 1;
        else
            p->sink_failed = 1;
    }
    p->spos = 0;
    return ok;
}

static void corrupt(psp_ppu *p)
{
    p->corrupt = 1;
    p->sink_failed = 1; /* nothing more is written; the capture has no trailer (v1 sink-failure rule) */
    p->spos = 0;
}

static int ascii_ok(const char *s, size_t lo, size_t hi)
{
    size_t n = 0;
    if (!s)
        return lo == 0;
    for (; s[n]; n++)
        if ((unsigned char)s[n] >= 0x80 || n >= hi)
            return 0;
    return n >= lo;
}

int psp_ppu_init(psp_ppu *p, const psp_cfg *cfg, const psp_limits *lim, const psp_mem *mem, const psp_out *out)
{
    uint64_t per;
    memset(p, 0, sizeof *p);
    if (cfg->ring_slots < 8 || cfg->ring_slots > (1u << 20) || (cfg->ring_slots & (cfg->ring_slots - 1)) ||
        cfg->lanes_max < 1 || cfg->lanes_max > 64 || cfg->cancel_after == 0)
        return PSP_P_EINVAL;
    per = (uint64_t)cfg->ring_slots * PSP_SLOT + PSP_CTL_BYTES;
    if (per > UINT64_MAX / cfg->lanes_max || per * cfg->lanes_max > cfg->mem_max)
        return PSP_P_ENOMEM; /* the whole budget is checked at init, with no overflow */
    p->cfg = *cfg;
    p->lim = *lim;
    p->mem = mem;
    p->out = out;
    p->scratch = malloc(MAX_CHUNK);
    return p->scratch ? PSP_P_OK : PSP_P_ENOMEM;
}

void psp_ppu_fini(psp_ppu *p)
{
    uint32_t i;
    for (i = 0; i < 32; i++) {
        free(p->images[i].descs);
        free(p->images[i].arena);
    }
    free(p->scratch);
    p->scratch = NULL;
}

/* the SPU_IMAGES entry size of an image whose strings have been validated (ASCII, bounded) */
static size_t image_bytes(uint32_t blen, const psp_desc *d, uint32_t n)
{
    size_t b = 4 + 2 + blen + 4, i;
    for (i = 0; i < n; i++)
        b += 12 + 2 + strlen(d[i].name) + 2 + (d[i].file ? strlen(d[i].file) : 0);
    return b;
}

int psp_image_add(psp_ppu *p, const uint8_t *build_id, uint32_t build_len, const psp_desc *d, uint32_t n,
                  uint32_t *index)
{
    psp_image *im;
    uint32_t i;
    size_t strings = 0, at = 0;
    /* validate everything before anything is kept, then copy it all: registration is all-or-nothing */
    if (p->sealed)
        return PSP_P_ESTOPPED;
    if (p->nimages >= p->lim.images || p->nimages >= 32)
        return PSP_P_ENOSPC;
    if (build_len < 1 || build_len > 64 || n < 1 || n > 4096)
        return PSP_P_EINVAL;
    for (i = 0; i < n; i++)
        if (!ascii_ok(d[i].name, 1, 512) || !ascii_ok(d[i].file, 0, 512))
            return PSP_P_EINVAL;
    if (4 + image_bytes(build_len, d, n) > MAX_CHUNK)
        return PSP_P_EINVAL; /* one image is one SPU_IMAGES entry: it must fit a chunk */
    for (i = 0; i < n; i++)
        strings += strlen(d[i].name) + 1 + (d[i].file ? strlen(d[i].file) : 0) + 1;
    im = &p->images[p->nimages];
    im->descs = malloc(sizeof *d * n);
    im->arena = malloc(strings);
    if (!im->descs || !im->arena) {
        free(im->descs);
        free(im->arena);
        im->descs = NULL;
        im->arena = NULL;
        return PSP_P_ENOMEM;
    }
    for (i = 0; i < n; i++) {
        size_t ln = strlen(d[i].name) + 1, lf = (d[i].file ? strlen(d[i].file) : 0) + 1;
        im->descs[i].ls_addr = d[i].ls_addr;
        im->descs[i].line = d[i].line;
        memcpy(im->arena + at, d[i].name, ln);
        im->descs[i].name = im->arena + at;
        at += ln;
        memcpy(im->arena + at, d[i].file ? d[i].file : "", lf);
        im->descs[i].file = im->arena + at;
        at += lf;
    }
    memcpy(im->build_id, build_id, build_len);
    im->build_len = build_len;
    im->ndesc = n;
    im->used = 1;
    *index = p->nimages++;
    return PSP_P_OK;
}

int psp_lane_create(psp_ppu *p, const char *name, uint32_t image, uint32_t owner, uint32_t *lane)
{
    uint32_t i;
    psp_lane *l;
    size_t ring = (size_t)p->cfg.ring_slots * PSP_SLOT, bytes = ring + PSP_CTL_BYTES;
    if (p->sealed)
        return PSP_P_ESTOPPED;
    if (image >= p->nimages || !ascii_ok(name, 1, 31))
        return PSP_P_EINVAL;
    if (p->lanes_created >= p->lim.lanes_life || p->lanes_live >= p->lim.lanes_live ||
        p->epochs_emitted + p->epochs_reserved + 1 > p->lim.epochs ||
        p->loss_emitted + p->loss_reserved + 1 > p->lim.loss_chunks)
        return PSP_P_ENOSPC;
    if (p->mem_used + bytes > p->cfg.mem_max)
        return PSP_P_ENOMEM; /* lanes not yet freed (sealed, or file-final but live) are still charged here */
    for (i = 0; i < p->cfg.lanes_max && p->lanes[i].state != PSP_L_FREE; i++)
        ;
    if (i == p->cfg.lanes_max)
        return PSP_P_ENOSPC;
    l = &p->lanes[i];
    memset(l, 0, sizeof *l);
    if (p->mem->alloc(p->mem->ctx, ring, &l->ea_ring) != 0)
        return PSP_P_ENOMEM;
    if (p->mem->alloc(p->mem->ctx, PSP_CTL_BYTES, &l->ea_ctl) != 0) {
        p->mem->free(p->mem->ctx, l->ea_ring, ring);
        return PSP_P_ENOMEM;
    }
    l->bytes = bytes;
    p->mem_used += bytes;
    l->lane_id = 1 + p->lanes_created;
    l->domain_id = 2 + p->lanes_created; /* 2..1025: unique, never reused */
    p->lanes_created++;
    p->lanes_live++;
    l->image = image;
    l->owner = owner;
    strcpy(l->name, name);
    l->res_epoch1 = 1;
    p->epochs_reserved++;
    if (!p->test_no_terminal) {
        l->res_terminal = 1;
        p->loss_reserved++;
    }
    l->next_req = 1;
    l->state = PSP_L_CREATED;
    *lane = i;
    return PSP_P_OK;
}

/* ---- declarations ---- */

static void declare(psp_ppu *p)
{
    uint32_t i, cnt = 0;
    size_t at = 0, b;
    for (i = 0; i < p->nimages; i++) {
        psp_image *im = &p->images[i];
        uint32_t j;
        if (im->emitted)
            continue;
        b = image_bytes(im->build_len, im->descs, im->ndesc);
        if (cnt && (cnt == 32 || p->spos + b > MAX_CHUNK)) {
            poke32(p, at, cnt);
            emit(p, C_SPU_IMAGES);
            cnt = 0;
        }
        if (!cnt) {
            at = p->spos;
            le32(p, 0);
        }
        le32(p, i);
        lestr(p, im->build_id, im->build_len);
        le32(p, im->ndesc);
        for (j = 0; j < im->ndesc; j++) {
            const psp_desc *d = &im->descs[j];
            le32(p, j);
            le32(p, d->ls_addr);
            le32(p, d->line);
            lestr(p, d->name, strlen(d->name));
            lestr(p, d->file, strlen(d->file));
        }
        im->emitted = 1;
        cnt++;
    }
    if (cnt) {
        poke32(p, at, cnt);
        emit(p, C_SPU_IMAGES);
    }
    /* DOMAIN then SPU_LANES in the same drain: the forward reference binds at once */
    for (int pass = 0; pass < 2; pass++) {
        cnt = 0;
        for (i = 0; i < p->cfg.lanes_max; i++) {
            psp_lane *l = &p->lanes[i];
            char dn[24];
            if (l->state != PSP_L_CREATED)
                continue;
            if (cnt == 64) {
                poke32(p, 0, cnt);
                emit(p, pass ? C_SPU_LANES : C_DOMAIN);
                cnt = 0;
            }
            if (!cnt)
                le32(p, 0);
            if (!pass) {
                le32(p, l->domain_id);
                le8(p, 1);
                le8(p, 32);
                le8(p, 1);
                le8(p, 0);
                le32(p, 1);
                le32(p, l->lane_id);
                le64(p, 0);
                le64(p, 0);
                le64(p, 0);
                le64(p, 0);
                snprintf(dn, sizeof dn, "spu-dec-%u", l->lane_id);
                lestr(p, dn, strlen(dn));
            } else {
                le32(p, l->lane_id);
                le32(p, l->domain_id);
                le32(p, l->image);
                le32(p, l->owner);
                le64(p, l->lane_id);
                lestr(p, l->name, strlen(l->name));
            }
            cnt++;
        }
        if (cnt) {
            poke32(p, 0, cnt);
            emit(p, pass ? C_SPU_LANES : C_DOMAIN);
        }
    }
    for (i = 0; i < p->cfg.lanes_max; i++)
        if (p->lanes[i].state == PSP_L_CREATED)
            p->lanes[i].state = PSP_L_DECLARED;
}

void psp_drain_declare(psp_ppu *p)
{
    p->cs++;
    declare(p);
    p->cs--;
}

/* ---- the replay of the accepted prefix (spec 3), for open_begins ---- */

static void go_unknown(psp_lane *l)
{
    l->sp = 0;
    l->known = 0;
    l->ever_unknown = 1;
}

static void replay(psp_lane *l, uint8_t kind, uint16_t desc, uint8_t depth)
{
    if (l->protocol)
        return;
    if (!l->known) {
        if (kind == PSP_K_BEGIN && depth == 0) {
            l->known = 1;
            l->sdesc[0] = desc;
            l->sdepth[0] = 0;
            l->sp = 1;
        }
        return;
    }
    if (kind == PSP_K_BEGIN) {
        if (depth != l->sp || l->sp == 64)
            l->protocol = 1;
        else {
            l->sdesc[l->sp] = desc;
            l->sdepth[l->sp++] = depth;
        }
    } else if (kind == PSP_K_MARK) {
        if (depth != l->sp)
            l->protocol = 1;
    } else if (l->sp && l->sdesc[l->sp - 1] == desc && l->sdepth[l->sp - 1] == depth) {
        l->sp--;
    } else {
        l->protocol = 1;
    }
}

/* ---- the ledger and FINAL ---- */

static void refund_bracket(psp_ppu *p, psp_lane *l, int tell_spu)
{
    if (!l->res_bracket)
        return;
    if (tell_spu)
        p->mem->st16(p->mem->ctx, l->ea_ctl + PSP_A_CAL_CANCEL, l->out_id);
    l->res_bracket = 0;
    p->brackets_file--;
    l->out_id = 0;
}

/* retire the lane from encoding; final_written only when the sink accepted the FINAL */
static void write_final(psp_ppu *p, psp_lane *l, uint32_t state)
{
    le32(p, 1);
    le32(p, l->lane_id);
    le8(p, 1);
    le8(p, (uint8_t)state);
    le16(p, 0);
    le64(p, l->emitted_next);
    le64(p, l->emitted_events);
    le64(p, l->emitted_lost);
    le32(p, (l->ever_unknown || l->protocol) ? UINT32_MAX : l->sp); /* writer placeholder when UNVERIFIABLE */
    l->final_written = emit(p, C_DOMAIN_FINAL);
    if (l->res_epoch1) {
        l->res_epoch1 = 0;
        p->epochs_reserved--;
    }
    if (l->res_terminal) {
        l->res_terminal = 0;
        p->loss_reserved--;
    }
    refund_bracket(p, l, 0);
    p->lanes_live--;
    l->fin_state = state;
    l->state = PSP_L_FINAL;
}

/* ---- the walk ---- */

typedef struct walk {
    uint32_t ev_n;
    uint64_t pl_first, pl_count;
    int have_br;
    uint64_t br_dec, br_tb0, br_tb1;
    int stop;
} walk;

/* emit the open SPU_EVENTS chunk; on acceptance replay it and count it */
static void close_events(psp_ppu *p, psp_lane *l, walk *w)
{
    uint32_t n = w->ev_n, i;
    if (!n)
        return;
    poke32(p, 16, n);
    w->ev_n = 0;
    if (!emit(p, C_SPU_EVENTS))
        return;
    for (i = 0; i < n; i++) {
        const uint8_t *e = p->scratch + 20 + 8 * (size_t)i;
        replay(l, e[6], (uint16_t)(e[4] | e[5] << 8), e[7]);
    }
    l->emitted_next += n;
    l->emitted_events += n;
}

/* a LOSS chunk that is not the reserved terminal: -1 when it would break the ledger */
static int flush_loss(psp_ppu *p, psp_lane *l, walk *w)
{
    uint64_t c = w->pl_count;
    if (!c)
        return 0;
    if (p->loss_emitted + p->loss_reserved + 1 > p->lim.loss_chunks)
        return -1;
    le32(p, l->lane_id);
    le8(p, 1);
    le8(p, 1);
    le16(p, 0);
    le64(p, w->pl_first);
    le64(p, c);
    le32(p, 0);
    w->pl_count = 0;
    if (emit(p, C_LOSS)) {
        p->loss_emitted++;
        l->emitted_next += c;
        l->emitted_lost += c;
        go_unknown(l);
    }
    return 0;
}

static void flush_bracket(psp_ppu *p, psp_lane *l, walk *w)
{
    if (!w->have_br)
        return;
    w->have_br = 0;
    le32(p, l->domain_id);
    le32(p, l->epoch);
    le64(p, l->brackets_epoch);
    le32(p, 1);
    le64(p, w->br_dec);
    le64(p, w->br_tb0);
    le64(p, w->br_tb1);
    if (emit(p, C_CALIBRATION)) {
        l->brackets_epoch++;
        l->res_bracket = 0; /* the reservation became an accepted bracket; brackets_file keeps counting it */
    }
}

/* finish the lane OVERFLOWED at its accepted prefix (a ledger refusal) */
static void overflow_finish(psp_ppu *p, psp_lane *l, walk *w)
{
    close_events(p, l, w);
    flush_bracket(p, l, w);
    w->pl_count = 0; /* not emitted: the FINAL describes only accepted data */
    if (l->res_bracket)
        refund_bracket(p, l, 1);
    write_final(p, l, PSP_FIN_OVERFLOWED);
    l->released = 1; /* no further access to this lane's MEMORY; its table entry stays ours until the lock exits */
    w->stop = 1;
}

static void walk_slots(psp_ppu *p, psp_lane *l, const uint8_t *s, uint64_t n)
{
    walk w;
    uint64_t i;
    const psp_image *im = &p->images[l->image];
    memset(&w, 0, sizeof w);
    for (i = 0; i < n && !w.stop && !p->sink_failed; i++, s += PSP_SLOT) {
        uint8_t kind = s[6];
        if (kind >= PSP_K_BEGIN && kind <= PSP_K_MARK) {
            uint16_t desc = psp_rd16(s + 4);
            if (s[7] > 63 || desc >= im->ndesc) {
                corrupt(p);
                return;
            }
            if (flush_loss(p, l, &w) != 0) {
                overflow_finish(p, l, &w);
                return;
            }
            if (p->sink_failed)
                break;
            if (!w.ev_n) {
                le32(p, l->lane_id);
                le32(p, l->epoch);
                le64(p, l->emitted_next);
                le32(p, 0);
            }
            le32(p, psp_rd32(s));
            le16(p, desc);
            le8(p, kind);
            le8(p, s[7]);
            w.ev_n++;
            if (w.ev_n == MAX_SPU_EVENTS)
                close_events(p, l, &w);
        } else if (kind == PSP_K_LOSS) {
            uint32_t c = psp_rd32(s);
            if (!c || s[4] || s[5] || s[7]) {
                corrupt(p);
                return;
            }
            close_events(p, l, &w);
            if (!w.pl_count)
                w.pl_first = l->emitted_next;
            w.pl_count += c;
        } else if (kind == PSP_K_EPOCH) {
            if (s[4] || s[5] || s[7]) {
                corrupt(p);
                return;
            }
            close_events(p, l, &w);
            if (flush_loss(p, l, &w) != 0) {
                overflow_finish(p, l, &w);
                return;
            }
            flush_bracket(p, l, &w); /* an old-epoch bracket never follows a newer EPOCH */
            if (p->sink_failed)
                break;
            if (l->res_bracket) {
                refund_bracket(p, l, 1);
                l->cancelled++;
            }
            if (p->epochs_emitted + p->epochs_reserved + 1 > p->lim.epochs) {
                overflow_finish(p, l, &w);
                return;
            }
            le32(p, 1);
            le32(p, l->domain_id);
            le32(p, l->epoch + 1);
            le8(p, 2);
            le8(p, 0);
            le8(p, 0);
            le8(p, 0);
            le64(p, psp_rd32(s));
            le64(p, p->mem->mftb(p->mem->ctx));
            if (emit(p, C_EPOCH)) {
                p->epochs_emitted++;
                l->epoch++;
                l->brackets_epoch = 0;
                go_unknown(l);
            }
        } else if (kind == PSP_K_CAL) {
            uint16_t id = psp_rd16(s + 4);
            if (!id || s[7]) {
                corrupt(p);
                return;
            }
            if (l->res_bracket && id == l->out_id && l->out_epoch == l->epoch) {
                w.have_br = 1;
                w.br_dec = psp_rd32(s);
                w.br_tb0 = l->out_tb0;
                w.br_tb1 = l->tb1;
                l->out_id = 0;
            } else {
                l->stale++;
            }
        } else if (kind == PSP_K_PAD) {
            if (psp_rd32(s) || s[4] || s[5] || s[7]) {
                corrupt(p);
                return;
            }
        } else {
            corrupt(p);
            return;
        }
    }
    if (w.stop || p->sink_failed) {
        p->spos = 0; /* a partly built chunk is dropped, not counted */
        return;
    }
    close_events(p, l, &w);
    if (flush_loss(p, l, &w) != 0) {
        overflow_finish(p, l, &w);
        return;
    }
    flush_bracket(p, l, &w);
}

/* ---- the snapshot (r3 F) and the per-lane coroutine ---- */

static int snap_valid(const psp_ppu *p, const psp_lane *l, const psp_snap *s, const uint8_t *line)
{
    uint32_t i;
    if (s->state > PSP_ST_CLOSED || (s->flags & ~PSP_F_KNOWN) || (s->flags & PSP_F_FAULT) || (s->w & 1) ||
        s->w < l->r || s->w - l->r > p->cfg.ring_slots || s->next_seq > PSP_SEQ_MAX)
        return 0; /* FAULT is never published (a local fault stops all DMA): its presence is corruption */
    if (s->gen == 0) { /* never started: the whole line is still zero */
        for (i = 0; i < 128; i++)
            if (line[i])
                return 0;
    } else if (!(s->gen & 1) || s->state == PSP_ST_NONE) {
        return 0;
    }
    if (l->have_good && (s->w < l->good.w || s->gen < l->good.gen || s->cal_answered < l->good.cal_answered ||
                         s->next_seq < l->good.next_seq || (l->good.state == PSP_ST_CLOSED && s->gen != l->good.gen)))
        return 0;
    if (line[PSP_B_CAL_ANSWERED - PSP_B + 2] || line[PSP_B_CAL_ANSWERED - PSP_B + 3])
        return 0;
    for (i = PSP_B_EPOCHS_WRITTEN - PSP_B + 4; i < PSP_B_GEN_END - PSP_B; i++)
        if (line[i])
            return 0;
    return 1;
}

static void parse_line(const uint8_t *b, psp_snap *s)
{
    s->gen = psp_rd64(b + PSP_B_GEN - PSP_B);
    s->w = psp_rd64(b + PSP_B_WRITE_IDX - PSP_B);
    s->next_seq = psp_rd64(b + PSP_B_NEXT_SEQ - PSP_B);
    s->older_lost = psp_rd64(b + PSP_B_OLDER_LOST - PSP_B);
    s->refused = psp_rd64(b + PSP_B_REFUSED - PSP_B);
    s->refused_calls = psp_rd64(b + PSP_B_REFUSED_CALLS - PSP_B);
    s->state = psp_rd32(b + PSP_B_STATE - PSP_B);
    s->flags = psp_rd32(b + PSP_B_FLAGS - PSP_B);
    s->cal_answered = psp_rd16(b + PSP_B_CAL_ANSWERED - PSP_B);
    s->epochs_written = psp_rd32(b + PSP_B_EPOCHS_WRITTEN - PSP_B);
}

static int step(psp_ppu *p, uint32_t i)
{
    psp_lane *l = &p->lanes[i];
    const psp_mem *m = p->mem;
    psp_snap s;
    uint64_t g;
    if (l->state < PSP_L_DECLARED || l->state == PSP_L_FINAL || l->released || p->sealed)
        return 1; /* not declared yet, or no longer the drainer's to touch */
    if (p->sink_failed) {
        l->dphase = 0;
        return 1; /* output stopped: nothing more is encoded and no lane memory is read */
    }
    switch (l->dphase) {
    case 0:
        l->g0 = m->ld64(m->ctx, l->ea_ctl + PSP_B_GEN);
        m->lwsync(m->ctx);
        l->dphase = 1;
        return 0;
    case 1:
        m->ld_line(m->ctx, l->ea_ctl + PSP_B, l->line);
        m->lwsync(m->ctx);
        l->dphase = 2;
        return 0;
    case 2:
        g = m->ld64(m->ctx, l->ea_ctl + PSP_B_GEN_END);
        m->lwsync(m->ctx);
        l->dphase = g == l->g0 ? 3 : 7;
        return 0;
    case 3:
        g = m->ld64(m->ctx, l->ea_ctl + PSP_B_GEN); /* the final first-word recheck */
        parse_line(l->line, &s);
        if (g != l->g0 || s.gen != l->g0 || psp_rd64(l->line + PSP_B_GEN_END - PSP_B) != l->g0) {
            l->dphase = 7;
            return 0;
        }
        m->lwsync(m->ctx); /* publication -> data: no ring load may pass the accepting recheck */
        if (!snap_valid(p, l, &s, l->line)) {
            corrupt(p);
            l->dphase = 0;
            return 1;
        }
        l->good = s;
        l->have_good = 1;
        l->w_snap = s.w;
        l->dphase = 4;
        return 0;
    case 4:
        if (l->epoch == 0 && l->good.state >= PSP_ST_STARTED) {
            le32(p, 1);
            le32(p, l->domain_id);
            le32(p, 1);
            le8(p, 1);
            le8(p, 0);
            le8(p, 0);
            le8(p, 0);
            le64(p, 0);
            le64(p, m->mftb(m->ctx));
            if (emit(p, C_EPOCH)) {
                p->epochs_reserved--;
                p->epochs_emitted++;
                l->res_epoch1 = 0;
                l->epoch = 1;
                l->known = 1;
                l->state = PSP_L_STARTED;
            }
            if (p->sink_failed)
                return 1;
        }
        if (l->w_snap > l->r) {
            uint64_t n = l->w_snap - l->r, first = l->r & (p->cfg.ring_slots - 1), a = p->cfg.ring_slots - first;
            uint8_t *buf = malloc((size_t)n * PSP_SLOT);
            if (!buf) {
                corrupt(p);
                return 1;
            }
            if (a > n)
                a = n;
            m->ld_slots(m->ctx, l->ea_ring + first * PSP_SLOT, buf, (size_t)a * PSP_SLOT);
            if (n > a)
                m->ld_slots(m->ctx, l->ea_ring, buf + a * PSP_SLOT, (size_t)(n - a) * PSP_SLOT);
            m->sync(m->ctx);
            m->isync(m->ctx);
            l->tb1 = m->mftb(m->ctx);
            walk_slots(p, l, buf, n);
            free(buf);
            if (l->state == PSP_L_FINAL || p->sink_failed) {
                l->dphase = 0;
                return 1; /* overflow-finished (released) or the sink failed: no further access */
            }
        }
        l->dphase = 5;
        return 0;
    case 5:
        m->lwsync(m->ctx); /* the copy loads complete before read_idx releases the space */
        m->st64(m->ctx, l->ea_ctl + PSP_A_READ_IDX, l->w_snap);
        l->r = l->w_snap;
        l->dphase = 6;
        return 0;
    case 6:
        if (l->good.state == PSP_ST_CLOSED) {
            if (l->res_bracket) {
                refund_bracket(p, l, 0);
                l->cancelled++;
            }
        } else if (l->res_bracket) {
            if (++l->out_waits >= p->cfg.cancel_after) {
                refund_bracket(p, l, 1);
                l->cancelled++;
            }
        } else if (l->epoch >= 1 && l->next_req <= PSP_REQ_MAX && p->brackets_file + 1 <= p->lim.brackets_file &&
                   l->brackets_epoch + 1 <= p->lim.brackets_epoch) {
            uint64_t tb0 = m->mftb(m->ctx);
            m->sync(m->ctx);
            m->st16(m->ctx, l->ea_ctl + PSP_A_CAL_REQ, (uint16_t)l->next_req);
            l->out_id = (uint16_t)l->next_req++;
            l->out_tb0 = tb0;
            l->out_epoch = l->epoch;
            l->out_waits = 0;
            l->res_bracket = 1;
            p->brackets_file++;
        }
        l->dphase = 0;
        l->tries = 0;
        return 1;
    default: /* 7: an inconsistent snapshot; retry from the first load, at most SNAP_TRIES per cycle */
        l->unstable++;
        l->dphase = ++l->tries < SNAP_TRIES ? 0 : 6;
        return 0;
    }
}

int psp_lane_step(psp_ppu *p, uint32_t i)
{
    int done;
    p->cs++;
    done = step(p, i);
    p->cs--;
    return done;
}

static void cycle(psp_ppu *p, uint32_t i)
{
    while (!step(p, i))
        ;
}

/* the end of a lane (inside the lock): a final drain cycle, then FINAL from the accepted prefix (r3 E) */
static void finish_lane(psp_ppu *p, uint32_t i)
{
    psp_lane *l = &p->lanes[i];
    if (l->state == PSP_L_FINAL || l->released)
        goto release;
    if (l->state >= PSP_L_DECLARED && !p->sink_failed)
        cycle(p, i);
    if (l->state == PSP_L_FINAL)
        goto release;
    if (!p->sink_failed && l->have_good && l->good.state == PSP_ST_CLOSED && l->r == l->good.w) {
        if (l->emitted_next + l->good.older_lost != l->good.next_seq) {
            corrupt(p); /* the producer's last word disagrees with what it published */
        } else if (l->good.older_lost) { /* the exact trailing range, from the reserved terminal LOSS */
            le32(p, l->lane_id);
            le8(p, 1);
            le8(p, 1);
            le16(p, 0);
            le64(p, l->emitted_next);
            le64(p, l->good.older_lost);
            le32(p, 0);
            if (emit(p, C_LOSS)) {
                if (l->res_terminal) {
                    l->res_terminal = 0;
                    p->loss_reserved--;
                }
                p->loss_emitted++;
                l->emitted_next += l->good.older_lost;
                l->emitted_lost += l->good.older_lost;
                go_unknown(l);
            }
        }
        write_final(p, l, (l->good.flags & (PSP_F_OVERFLOWED | PSP_F_GEN_EXHAUSTED)) ? PSP_FIN_OVERFLOWED
                                                                                    : PSP_FIN_FINALIZED);
    } else {
        write_final(p, l, PSP_FIN_ABANDONED); /* not accepted when the sink has failed: final_written stays 0 */
    }
release:
    l->released = 1; /* the drainer's last access to the lane's memory; the entry is freed only outside the lock */
}

void psp_drain_ends(psp_ppu *p)
{
    uint32_t i;
    p->cs++;
    for (i = 0; i < p->cfg.lanes_max; i++)
        if (p->lanes[i].state != PSP_L_FREE && p->lanes[i].end_req && !p->lanes[i].released)
            finish_lane(p, i);
    p->cs--;
}

void psp_drain(psp_ppu *p)
{
    uint32_t i;
    if (p->sealed)
        return;
    psp_drain_declare(p);
    for (i = 0; i < p->cfg.lanes_max; i++)
        if (p->lanes[i].state != PSP_L_FREE) {
            p->cs++;
            cycle(p, i);
            p->cs--;
        }
    psp_drain_ends(p);
}

void psp_lane_end_request(psp_ppu *p, uint32_t lane) { p->lanes[lane].end_req = 1; }

int psp_lane_end_poll(psp_ppu *p, uint32_t lane, uint32_t max_waits)
{
    psp_lane *l = &p->lanes[lane];
    int sealed;
    if (p->cs)
        return PSP_P_EBUSY; /* the lane-table lock is held: no free or reuse inside it */
    if (!l->released) {
        if (++l->end_waits > max_waits) {
            l->end_waits = 0;
            return PSP_P_ETIMEDOUT; /* memory kept and still charged; the caller may retry */
        }
        return PSP_P_EBUSY;
    }
    sealed = l->file_final_by_seal;
    p->mem->free(p->mem->ctx, l->ea_ring, l->bytes - PSP_CTL_BYTES);
    p->mem->free(p->mem->ctx, l->ea_ctl, PSP_CTL_BYTES);
    p->mem_used -= l->bytes; /* bytes are refunded only on the actual free */
    memset(l, 0, sizeof *l);
    return sealed ? PSP_P_ESTOPPED : PSP_P_OK;
}

int psp_lane_end(psp_ppu *p, uint32_t lane)
{
    psp_lane_end_request(p, lane);
    psp_drain(p);
    if (p->sealed) { /* the seal finalized and released it already */
        p->cs++;
        finish_lane(p, lane);
        p->cs--;
    }
    return psp_lane_end_poll(p, lane, 1);
}

void psp_seal(psp_ppu *p)
{
    uint32_t i;
    if (p->sealed)
        return;
    p->cs++;
    declare(p);
    for (i = 0; i < p->cfg.lanes_max; i++) {
        psp_lane *l = &p->lanes[i];
        if (l->state == PSP_L_FREE)
            continue;
        if (l->state != PSP_L_FINAL && !l->released)
            l->file_final_by_seal = 1;
        l->sealed_unfreed = 1; /* set before the release: the release is the last write the seal makes */
        finish_lane(p, i);
    }
    p->sealed = 1;
    p->cs--;
}

int psp_lane_stats_get(psp_ppu *p, uint32_t lane, int post_join, psp_lane_stats *st)
{
    psp_lane *l = &p->lanes[lane];
    memset(st, 0, sizeof *st);
    if (l->state == PSP_L_FREE)
        return PSP_P_EINVAL;
    st->final_written = l->final_written; /* FROZEN_AT_FINAL: only a FINAL the sink accepted */
    st->fin_state = l->final_written ? l->fin_state : 0;
    st->next_seq = l->emitted_next;
    st->events = l->emitted_events;
    st->lost = l->emitted_lost;
    st->stale = l->stale;
    st->cancelled = l->cancelled;
    st->unstable = l->unstable;
    if (post_join && l->released) { /* the caller asserts a successful join; the drainer is done with it */
        uint8_t b[128];
        p->mem->ld_line(p->mem->ctx, l->ea_ctl + PSP_B, b);
        parse_line(b, &st->post_join);
        st->have_post_join = 1;
    }
    return PSP_P_OK;
}
