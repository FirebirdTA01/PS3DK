/* PS3DKROL container version 1, physical foundation only.
 *
 * LE wire layout (never cast disk bytes to a C struct):
 *   0: 4096-byte config; 4096,8192: alternate 4096-byte roots;
 *   12288: retained_slots+1 equal slots.
 * Config: magic[8],version:u32,page:u32,file:u64,cap:u64,N:u32,
 *         checkpoint:u32,payload:u32,slot:u32,session[16],zero,CRC@4092.
 * Root: magic[8],generation:u64,count:u32,zero:u32,first_segment:u64,
 *       next_segment:u64,first_chunk:u64,next_chunk:u64,evicted_segments:u64,
 *       evicted_chunks:u64,evicted_bytes:u64,session[16],entries@96,zero,CRC.
 * Entry (40): physical:u32,checkpoint:u32,payload:u32,zero:u32,
 *             segment:u64,first_chunk:u64,next_chunk:u64.
 * Slot header (96): magic[8],session[16],segment:u64,generation:u64,
 *       first_chunk:u64,next_chunk:u64,checkpoint:u32,payload:u32,
 *       checkpoint_crc:u32,payload_crc:u32,zero:u64,header_crc:u32,zero[12].
 * Then actual checkpoint bytes, actual payload bytes, zero padding.
 * Footer (last 64): magic[8],segment:u64,generation:u64,first_chunk:u64,
 *       next_chunk:u64,checkpoint_crc:u32,payload_crc:u32,content_crc:u32,
 *       zero[8],footer_crc:u32. CRC is IEEE CRC32 over preceding bytes.
 *
 * Every successful commit writes/syncs only the spare slot, then writes/syncs
 * the alternate root. The current root is never invalidated first. A prior
 * root can reference the spare: recovery validates each entire chain and
 * never merges chains. Adapter ordering/durability must be established by the
 * integrator; no atomic-write size or filesystem durability is assumed here. */
#include "roll_store.h"
#include <string.h>

#define PAGE 4096u
#define PREFIX 12288u
#define SLOT_HEAD 96u
#define SLOT_FOOT 64u
#define ROOT_HEAD 96u
#define ENTRY_BYTES 40u
#define MAX_SLOTS 64u
#define MAX_CHUNK 1048576u
#define READY UINT32_C(0x50465231)

typedef struct {
    uint32_t physical, checkpoint, payload;
    uint64_t segment, first, next;
} entry;
typedef struct {
    pfr_view view;
    entry entries[MAX_SLOTS];
} root;
struct pfr_store {
    pfr_io io;
    pfr_config cfg;
    pfr_layout layout;
    uint8_t *scratch;
    root current;
    uint32_t ready, failed;
};

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p)
{ return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static void w32(uint8_t *p, uint32_t n)
{ unsigned i; for (i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8u * i)); }
static void w64(uint8_t *p, uint64_t n)
{ w32(p, (uint32_t)n); w32(p + 4, (uint32_t)(n >> 32)); }
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    unsigned j;
    while (n--) {
        c ^= *p++;
        for (j = 0; j < 8; ++j) c = (c >> 1) ^ (UINT32_C(0xedb88320) & (0u - (c & 1u)));
    }
    return ~c;
}
static int zeros(const uint8_t *p, size_t n)
{ while (n--) if (*p++) return 0; return 1; }
static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!an || !bn) return 0;
    return x <= y ? y - x < an : x - y < bn;
}
static int io_read(pfr_store *s, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = buf;
    if (off > s->layout.file_bytes || (uint64_t)n > s->layout.file_bytes - off) return PFR_CORRUPT;
    while (n) {
        int64_t got = s->io.read_at(s->io.ctx, off, p, n);
        if (got <= 0 || (uint64_t)got > (uint64_t)n) return PFR_IO;
        p += (size_t)got; off += (uint64_t)got; n -= (size_t)got;
    }
    return PFR_OK;
}
static int io_write(pfr_store *s, uint64_t off, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    if (off > s->layout.file_bytes || (uint64_t)n > s->layout.file_bytes - off) return PFR_IO;
    while (n) {
        int64_t got = s->io.write_at(s->io.ctx, off, p, n);
        if (got <= 0 || (uint64_t)got > (uint64_t)n) return PFR_IO;
        p += (size_t)got; off += (uint64_t)got; n -= (size_t)got;
    }
    return PFR_OK;
}
static int poison(pfr_store *s) { s->failed = 1; return PFR_IO; }
static uint64_t slot_offset(const pfr_store *s, uint32_t slot)
{ return (uint64_t)PREFIX + (uint64_t)slot * s->layout.slot_bytes; }
static uint64_t root_offset(uint64_t generation)
{ return (uint64_t)PAGE + (generation & 1u) * PAGE; }

size_t pfr_state_bytes(void) { return sizeof(pfr_store); }
int pfr_plan(const pfr_config *c, pfr_layout *l)
{
    uint64_t slot, total;
    if (!c || !l || c->retained_slots < 2 || c->retained_slots > MAX_SLOTS || c->payload_bytes < 20)
        return PFR_INVALID;
    slot = (uint64_t)SLOT_HEAD + c->checkpoint_bytes + c->payload_bytes + SLOT_FOOT;
    slot = (slot + PAGE - 1u) & ~(uint64_t)(PAGE - 1u);
    if (slot > UINT32_MAX || slot > SIZE_MAX) return PFR_SPACE;
    /* At most 65 slots of <2^32 bytes, so multiplication cannot overflow u64. */
    total = PREFIX + ((uint64_t)c->retained_slots + 1u) * slot;
    if (total > c->max_bytes) return PFR_SPACE;
    l->slot_bytes = (uint32_t)slot;
    l->file_bytes = total;
    l->scratch_bytes = (size_t)slot;
    return PFR_OK;
}

static int prepare(pfr_store *s, const pfr_io *io, const pfr_config *c, void *scratch, size_t n)
{
    pfr_layout layout;
    int rc;
    if (!s || !io || !io->read_at || !io->size || !scratch) return PFR_INVALID;
    rc = pfr_plan(c, &layout);
    if (rc != PFR_OK) return rc;
    if (n < layout.scratch_bytes) return PFR_SPACE;
    if (overlaps(s, sizeof *s, scratch, n) || overlaps(s, sizeof *s, c, sizeof *c)
        || overlaps(s, sizeof *s, io, sizeof *io)) return PFR_INVALID;
    memset(s, 0, sizeof *s);
    s->io = *io; s->cfg = *c; s->layout = layout; s->scratch = scratch;
    s->failed = 1;
    return PFR_OK;
}
static void config_encode(pfr_store *s)
{
    uint8_t *p = s->scratch;
    memset(p, 0, PAGE);
    memcpy(p, "PS3DKROL", 8); w32(p + 8, 1); w32(p + 12, PAGE);
    w64(p + 16, s->layout.file_bytes); w64(p + 24, s->cfg.max_bytes);
    w32(p + 32, s->cfg.retained_slots); w32(p + 36, s->cfg.checkpoint_bytes);
    w32(p + 40, s->cfg.payload_bytes); w32(p + 44, s->layout.slot_bytes);
    memcpy(p + 48, s->cfg.session, 16);
    w32(p + PAGE - 4, crc32(p, PAGE - 4));
}
static int config_check(const pfr_store *s)
{
    const uint8_t *p = s->scratch;
    return memcmp(p, "PS3DKROL", 8) == 0 && u32(p + 8) == 1 && u32(p + 12) == PAGE
        && u64(p + 16) == s->layout.file_bytes && u64(p + 24) == s->cfg.max_bytes
        && u32(p + 32) == s->cfg.retained_slots && u32(p + 36) == s->cfg.checkpoint_bytes
        && u32(p + 40) == s->cfg.payload_bytes && u32(p + 44) == s->layout.slot_bytes
        && memcmp(p + 48, s->cfg.session, 16) == 0 && zeros(p + 64, PAGE - 68)
        && u32(p + PAGE - 4) == crc32(p, PAGE - 4);
}
static void root_encode(pfr_store *s, const root *r)
{
    uint8_t *p = s->scratch;
    const pfr_view *v = &r->view;
    uint32_t i;
    memset(p, 0, PAGE);
    memcpy(p, "PFRROOT1", 8); w64(p + 8, v->generation); w32(p + 16, v->retained_segments);
    w64(p + 24, v->first_segment); w64(p + 32, v->next_segment);
    w64(p + 40, v->first_chunk); w64(p + 48, v->next_chunk);
    w64(p + 56, v->evicted_segments); w64(p + 64, v->evicted_chunks);
    w64(p + 72, v->evicted_payload_bytes); memcpy(p + 80, s->cfg.session, 16);
    for (i = 0; i < v->retained_segments; ++i) {
        const entry *e = &r->entries[i]; uint8_t *q = p + ROOT_HEAD + i * ENTRY_BYTES;
        w32(q, e->physical); w32(q + 4, e->checkpoint); w32(q + 8, e->payload);
        w64(q + 16, e->segment); w64(q + 24, e->first); w64(q + 32, e->next);
    }
    w32(p + PAGE - 4, crc32(p, PAGE - 4));
}
static int root_decode(const pfr_store *s, root *r, uint32_t which)
{
    const uint8_t *p = s->scratch;
    pfr_view *v = &r->view;
    uint32_t i, j;
    uint64_t next;
    memset(r, 0, sizeof *r);
    if (memcmp(p, "PFRROOT1", 8) || u32(p + PAGE - 4) != crc32(p, PAGE - 4)
        || u32(p + 20) || memcmp(p + 80, s->cfg.session, 16)) return PFR_CORRUPT;
    v->generation = u64(p + 8); v->retained_segments = u32(p + 16);
    v->first_segment = u64(p + 24); v->next_segment = u64(p + 32);
    v->first_chunk = u64(p + 40); v->next_chunk = u64(p + 48);
    v->evicted_segments = u64(p + 56); v->evicted_chunks = u64(p + 64);
    v->evicted_payload_bytes = u64(p + 72);
    if (!v->generation || (v->generation & 1u) != which || v->next_segment == UINT64_MAX
        || v->generation != v->next_segment + 1 || v->retained_segments > s->cfg.retained_slots
        || v->first_segment > v->next_segment || v->next_segment - v->first_segment != v->retained_segments
        || v->first_segment != v->evicted_segments || v->first_chunk != v->evicted_chunks
        || v->first_chunk > v->next_chunk) return PFR_CORRUPT;
    if (v->evicted_segments <= UINT64_MAX / s->cfg.payload_bytes
        && v->evicted_payload_bytes > v->evicted_segments * s->cfg.payload_bytes) return PFR_CORRUPT;
    if (!v->retained_segments && (v->generation != 1 || v->first_chunk || v->next_chunk || v->evicted_payload_bytes))
        return PFR_CORRUPT;
    if (v->next_segment < s->cfg.retained_slots) {
        if (v->retained_segments != v->next_segment) return PFR_CORRUPT;
    } else if (v->retained_segments != s->cfg.retained_slots) return PFR_CORRUPT;
    next = v->first_chunk;
    for (i = 0; i < v->retained_segments; ++i) {
        entry *e = &r->entries[i]; const uint8_t *q = p + ROOT_HEAD + i * ENTRY_BYTES;
        e->physical = u32(q); e->checkpoint = u32(q + 4); e->payload = u32(q + 8);
        e->segment = u64(q + 16); e->first = u64(q + 24); e->next = u64(q + 32);
        if (e->physical > s->cfg.retained_slots || e->checkpoint > s->cfg.checkpoint_bytes
            || e->payload > s->cfg.payload_bytes || u32(q + 12) || e->segment != v->first_segment + i
            || e->first != next || e->next < e->first || e->next - e->first > e->payload / 20u)
            return PFR_CORRUPT;
        for (j = 0; j < i; ++j) if (e->physical == r->entries[j].physical) return PFR_CORRUPT;
        next = e->next;
    }
    if (next != v->next_chunk || !zeros(p + ROOT_HEAD + v->retained_segments * ENTRY_BYTES,
        PAGE - 4u - ROOT_HEAD - v->retained_segments * ENTRY_BYTES)) return PFR_CORRUPT;
    return PFR_OK;
}
static int chunks(const uint8_t *p, size_t n, uint64_t first, uint64_t *next)
{
    uint64_t seq = first;
    while (n) {
        uint32_t len; size_t total;
        if (n < 20) return PFR_INVALID;
        len = u32(p + 4);
        if (len > MAX_CHUNK) return PFR_INVALID;
        total = (size_t)len + 20u;
        if (total > n || seq == UINT64_MAX || u64(p + 8) != seq
            || u32(p + total - 4) != crc32(p, total - 4)) return PFR_INVALID;
        ++seq; p += total; n -= total;
    }
    *next = seq;
    return PFR_OK;
}
static int slot_read(pfr_store *s, const entry *e)
{
    uint8_t *p = s->scratch, *f = p + s->layout.slot_bytes - SLOT_FOOT;
    uint32_t cc, pc;
    size_t used = SLOT_HEAD + (size_t)e->checkpoint + e->payload;
    uint64_t next;
    int rc = io_read(s, slot_offset(s, e->physical), p, s->layout.slot_bytes);
    if (rc != PFR_OK) return rc;
    if (used > s->layout.slot_bytes - SLOT_FOOT) return PFR_CORRUPT;
    cc = crc32(p + SLOT_HEAD, e->checkpoint);
    pc = crc32(p + SLOT_HEAD + e->checkpoint, e->payload);
    if (memcmp(p, "PFRSLOT1", 8) || memcmp(p + 8, s->cfg.session, 16)
        || u64(p + 24) != e->segment || e->segment > UINT64_MAX - 2
        || u64(p + 32) != e->segment + 2 || u64(p + 40) != e->first || u64(p + 48) != e->next
        || u32(p + 56) != e->checkpoint || u32(p + 60) != e->payload
        || u32(p + 64) != cc || u32(p + 68) != pc || !zeros(p + 72, 8)
        || u32(p + 80) != crc32(p, 80) || !zeros(p + 84, 12)
        || !zeros(p + used, s->layout.slot_bytes - SLOT_FOOT - used)
        || memcmp(f, "PFRCMIT1", 8) || u64(f + 8) != e->segment || u64(f + 16) != e->segment + 2
        || u64(f + 24) != e->first || u64(f + 32) != e->next || u32(f + 40) != cc || u32(f + 44) != pc
        || u32(f + 48) != crc32(p, s->layout.slot_bytes - SLOT_FOOT) || !zeros(f + 52, 8)
        || u32(f + 60) != crc32(f, 60)) return PFR_CORRUPT;
    if (chunks(p + SLOT_HEAD + e->checkpoint, e->payload, e->first, &next) != PFR_OK || next != e->next)
        return PFR_CORRUPT;
    return PFR_OK;
}

int pfr_format(pfr_store *s, const pfr_io *io, const pfr_config *c, void *scratch, size_t n)
{
    uint64_t size;
    int rc = prepare(s, io, c, scratch, n);
    if (rc != PFR_OK) return rc;
    if (!s->io.write_at || !s->io.resize || !s->io.sync) return PFR_INVALID;
    if (s->io.size(s->io.ctx, &size)) return poison(s);
    if (size) return PFR_INVALID;
    if (s->io.resize(s->io.ctx, s->layout.file_bytes) || s->io.size(s->io.ctx, &size)
        || size != s->layout.file_bytes) return poison(s);
    config_encode(s);
    if (io_write(s, 0, s->scratch, PAGE) != PFR_OK) return poison(s);
    memset(s->scratch, 0, PAGE);
    if (io_write(s, PAGE, s->scratch, PAGE) != PFR_OK || io_write(s, 2u * PAGE, s->scratch, PAGE) != PFR_OK
        || s->io.sync(s->io.ctx)) return poison(s);
    s->current.view.generation = 1;
    root_encode(s, &s->current);
    if (io_write(s, root_offset(1), s->scratch, PAGE) != PFR_OK || s->io.sync(s->io.ctx)) return poison(s);
    s->ready = READY; s->failed = 0;
    return PFR_OK;
}
int pfr_open(pfr_store *s, const pfr_io *io, const pfr_config *c, void *scratch, size_t n)
{
    root candidate[2];
    int valid[2] = {0, 0}, missing[2] = {0, 0}, rc, pick;
    uint32_t k, j; uint64_t size;
    rc = prepare(s, io, c, scratch, n);
    if (rc != PFR_OK) return rc;
    if (s->io.size(s->io.ctx, &size)) return PFR_IO;
    if (size != s->layout.file_bytes) return PFR_CORRUPT;
    rc = io_read(s, 0, s->scratch, PAGE);
    if (rc != PFR_OK) return rc;
    if (!config_check(s)) return PFR_CORRUPT;
    for (k = 0; k < 2; ++k) {
        rc = io_read(s, PAGE + (uint64_t)k * PAGE, s->scratch, PAGE);
        if (rc != PFR_OK) return rc;
        missing[k] = zeros(s->scratch, PAGE);
        if (root_decode(s, &candidate[k], k) != PFR_OK) continue;
        valid[k] = 1;
        for (j = 0; j < candidate[k].view.retained_segments; ++j) {
            rc = slot_read(s, &candidate[k].entries[j]);
            if (rc == PFR_IO) return rc;
            if (rc != PFR_OK) { valid[k] = 0; break; }
        }
    }
    if (!valid[0] && !valid[1]) return PFR_CORRUPT;
    pick = !valid[0] ? 1 : !valid[1] ? 0 : candidate[1].view.generation > candidate[0].view.generation;
    s->current = candidate[pick];
    s->current.view.recovered = (uint32_t)((!valid[0] && !missing[0]) || (!valid[1] && !missing[1]));
    /* A zero alternate root is normal only for the initial empty container. */
    if ((missing[0] || missing[1]) && s->current.view.generation != 1) s->current.view.recovered = 1;
    s->ready = READY; s->failed = 0;
    return PFR_OK;
}
int pfr_commit(pfr_store *s, const void *checkpoint, size_t cn, const void *payload, size_t pn)
{
    root next;
    entry *e;
    uint32_t slot, i, count, cc, pc;
    uint64_t frontier;
    uint8_t *p, *f;
    if (!s || s->ready != READY || s->failed) return PFR_FAILED;
    if (!s->io.write_at || !s->io.sync || (!checkpoint && cn) || (!payload && pn)) return PFR_INVALID;
    if (cn > s->cfg.checkpoint_bytes || pn > s->cfg.payload_bytes) return PFR_SPACE;
    if (overlaps(s->scratch, s->layout.scratch_bytes, checkpoint, cn)
        || overlaps(s->scratch, s->layout.scratch_bytes, payload, pn)
        || overlaps(s, sizeof *s, checkpoint, cn) || overlaps(s, sizeof *s, payload, pn)) return PFR_INVALID;
    if (s->current.view.generation == UINT64_MAX) return PFR_SPACE;
    if (chunks(payload, pn, s->current.view.next_chunk, &frontier) != PFR_OK) return PFR_INVALID;
    next = s->current;
    /* Choose an unreferenced slot BEFORE removing the oldest entry. */
    for (slot = 0; slot <= s->cfg.retained_slots; ++slot) {
        for (i = 0; i < next.view.retained_segments; ++i) if (next.entries[i].physical == slot) break;
        if (i == next.view.retained_segments) break;
    }
    if (slot > s->cfg.retained_slots) return PFR_CORRUPT;
    count = next.view.retained_segments;
    if (count == s->cfg.retained_slots) {
        const entry *old = &next.entries[0];
        if (next.view.evicted_payload_bytes > UINT64_MAX - old->payload) return PFR_SPACE;
        ++next.view.evicted_segments;
        next.view.evicted_chunks = old->next;
        next.view.evicted_payload_bytes += old->payload;
        memmove(next.entries, next.entries + 1, (count - 1u) * sizeof(entry));
        --count;
    }
    e = &next.entries[count];
    e->physical = slot; e->checkpoint = (uint32_t)cn; e->payload = (uint32_t)pn;
    e->segment = next.view.next_segment; e->first = next.view.next_chunk; e->next = frontier;
    ++next.view.generation; ++next.view.next_segment;
    next.view.next_chunk = frontier; next.view.retained_segments = count + 1u;
    next.view.first_segment = next.entries[0].segment; next.view.first_chunk = next.entries[0].first;
    p = s->scratch; f = p + s->layout.slot_bytes - SLOT_FOOT;
    memset(p, 0, s->layout.slot_bytes);
    if (cn) memcpy(p + SLOT_HEAD, checkpoint, cn);
    if (pn) memcpy(p + SLOT_HEAD + cn, payload, pn);
    cc = crc32(p + SLOT_HEAD, cn); pc = crc32(p + SLOT_HEAD + cn, pn);
    memcpy(p, "PFRSLOT1", 8); memcpy(p + 8, s->cfg.session, 16);
    w64(p + 24, e->segment); w64(p + 32, next.view.generation);
    w64(p + 40, e->first); w64(p + 48, e->next); w32(p + 56, e->checkpoint); w32(p + 60, e->payload);
    w32(p + 64, cc); w32(p + 68, pc); w32(p + 80, crc32(p, 80));
    memcpy(f, "PFRCMIT1", 8); w64(f + 8, e->segment); w64(f + 16, next.view.generation);
    w64(f + 24, e->first); w64(f + 32, e->next); w32(f + 40, cc); w32(f + 44, pc);
    w32(f + 48, crc32(p, s->layout.slot_bytes - SLOT_FOOT)); w32(f + 60, crc32(f, 60));
    if (io_write(s, slot_offset(s, slot), p, s->layout.slot_bytes) != PFR_OK || s->io.sync(s->io.ctx))
        return poison(s);
    root_encode(s, &next);
    if (io_write(s, root_offset(next.view.generation), p, PAGE) != PFR_OK || s->io.sync(s->io.ctx)) return poison(s);
    s->current = next;
    return PFR_OK;
}
int pfr_get_view(const pfr_store *s, pfr_view *v)
{
    if (!s || s->ready != READY || s->failed) return PFR_FAILED;
    if (!v) return PFR_INVALID;
    *v = s->current.view;
    return PFR_OK;
}
int pfr_get_segment(pfr_store *s, uint32_t index, pfr_segment *seg)
{
    const entry *e; int rc;
    if (!s || s->ready != READY || s->failed) return PFR_FAILED;
    if (!seg) return PFR_INVALID;
    if (index >= s->current.view.retained_segments) return PFR_END;
    e = &s->current.entries[index];
    rc = slot_read(s, e);
    if (rc != PFR_OK) return rc;
    seg->segment = e->segment; seg->first_chunk = e->first; seg->next_chunk = e->next;
    seg->checkpoint_bytes = e->checkpoint; seg->payload_bytes = e->payload;
    seg->checkpoint = s->scratch + SLOT_HEAD; seg->payload = seg->checkpoint + e->checkpoint;
    return PFR_OK;
}
