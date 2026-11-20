/* Cold physical identity/framing evidence only. Never semantic enrollment
 * authority: the runtime must separately have sealed, closed and joined OK. */
#include "roll_retention_inspect.h"
#include <string.h>

#define PFI_PAGE 4096u
#define PFI_MAX_CHUNK 1048576u

static uint16_t r16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t r32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p)
{ return r32(p) | (uint64_t)r32(p + 4) << 32; }
static int zeroes(const uint8_t *p, size_t n)
{ while (n--) if (*p++) return 0; return 1; }
static uint32_t crc_add(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i; unsigned bit;
    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return crc;
}
static uint32_t crc_bytes(const uint8_t *p, size_t n)
{ return ~crc_add(UINT32_MAX, p, n); }
static int range_valid(const void *p, size_t n)
{ return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    if (!range_valid(a, an) || !range_valid(b, bn)) return 1;
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static int read_exact(const pfr_io *io, uint64_t extent, uint64_t off, void *dst, size_t n)
{
    uint8_t *p = dst;
    if (off > extent || (uint64_t)n > extent - off) return PFI_CORRUPT;
    while (n) {
        int64_t got = io->read_at(io->ctx, off, p, n);
        if (got <= 0 || (uint64_t)got > (uint64_t)n) return PFI_IO;
        off += (uint64_t)got; p += (size_t)got; n -= (size_t)got;
    }
    return PFI_OK;
}
static int physical_error(int rc)
{
    if (rc == PFR_IO) return PFI_IO;
    if (rc == PFR_SPACE) return PFI_SPACE;
    return PFI_CORRUPT;
}

int pfi_measure(const pfr_io *io, const pfi_limits *limits, void *work,
                size_t bytes, pfi_requirements *out)
{
    pfi_requirements q; uint64_t extent; uint8_t *p = work; int rc;
    if (!io || !io->read_at || !io->size || !limits || !work || !out) return PFI_INVALID;
    if (bytes < PFI_WORK_BYTES) return PFI_SPACE;
    if (overlaps(work, bytes, io, sizeof *io) || overlaps(work, bytes, limits, sizeof *limits)
        || overlaps(work, bytes, out, sizeof *out)) return PFI_INVALID;
    if (io->size(io->ctx, &extent)) return PFI_IO;
    if (extent > limits->max_file_bytes) return PFI_SPACE;
    if (extent < PFI_PAGE) return PFI_CORRUPT;
    rc = read_exact(io, extent, 0, p, PFI_PAGE); if (rc) return rc;
    if (memcmp(p, "PS3DKROL", 8) || r32(p + 12) != PFI_PAGE
        || !zeroes(p + 64, PFI_PAGE - 68)
        || crc_bytes(p, PFI_PAGE - 4) != r32(p + PFI_PAGE - 4)) return PFI_CORRUPT;
    if (r32(p + 8) != 2) return PFI_UNSUPPORTED;
    memset(&q, 0, sizeof q);
    q.config.max_bytes = r64(p + 24); q.config.retained_slots = r32(p + 32);
    q.config.checkpoint_bytes = r32(p + 36); q.config.payload_bytes = r32(p + 40);
    memcpy(q.config.session, p + 48, 16);
    if (r32(p + 44) > limits->max_slot_bytes
        || q.config.checkpoint_bytes > limits->max_checkpoint_bytes) return PFI_SPACE;
    if (pfr_plan(&q.config, &q.layout) != PFR_OK) return PFI_CORRUPT;
    if (q.layout.file_bytes != extent || r64(p + 16) != extent
        || r32(p + 44) != q.layout.slot_bytes) return PFI_CORRUPT;
    if (q.layout.slot_bytes > limits->max_slot_bytes) return PFI_SPACE;
    q.store_bytes = pfr_state_bytes();
    memcpy(out, &q, sizeof q); return PFI_OK;
}

typedef struct {
    const char *app; size_t app_bytes; uint64_t session;
    uint8_t original[16]; uint32_t kind, flags; int have_original, have_identity;
} identity;

static int original_header(identity *id, const uint8_t *p, uint32_t kind)
{
    uint32_t flags = r32(p + 12), base = flags & ~UINT32_C(512);
    if (memcmp(p, "PS3DKPRF", 8)) return PFI_CORRUPT;
    if (r16(p + 8) != 1 || r16(p + 10) != 0) return PFI_UNSUPPORTED;
    if ((kind == PFI_CHECKPOINT_PPU && base != 0)
        || (kind == PFI_CHECKPOINT_COMBINED
            && ((flags & ~UINT32_C(711))
                || ((flags & 7) != 1 && (flags & 7) != 3 && (flags & 7) != 7))))
        return PFI_UNSUPPORTED;
    if (id->have_original) {
        if (id->kind != kind || memcmp(id->original, p, 16)) return PFI_CORRUPT;
    } else {
        memcpy(id->original, p, 16); id->kind = kind; id->flags = flags; id->have_original = 1;
    }
    return PFI_OK;
}
static int header_identity(identity *id, const uint8_t *p, size_t n)
{
    unsigned i;
    for (i = 0; i < 6; ++i) {
        size_t len;
        if (n < 2) return PFI_CORRUPT;
        len = r16(p); p += 2; n -= 2;
        if (len > n) return PFI_CORRUPT;
        if (!i && (len != id->app_bytes || memcmp(p, id->app, len))) return PFI_CORRUPT;
        p += len; n -= len;
    }
    if (n != 24 || r16(p + 2) || r64(p + 4) != id->session) return PFI_CORRUPT;
    id->have_identity = 1; return PFI_OK;
}
/* Only the admitted PPU/SPU/AUTO/FIFO/WINDOWS schemas have cold inspection.
 * This is a family/flag gate, not validation of the bodies of these chunks. */
static int chunk_type(uint32_t type, uint32_t flags, int metadata)
{
    switch (type) {
    case 1: case 2: case 3: case 4: case 7: return PFI_OK;
    case 5: case 6: return metadata ? PFI_CORRUPT : PFI_OK;
    case 8: case 9: case 10: case 11: case 12: case 15: case 16:
        return (flags & 1) ? PFI_OK : PFI_UNSUPPORTED;
    case 13: return metadata ? PFI_CORRUPT : ((flags & 1) ? PFI_OK : PFI_UNSUPPORTED);
    case 17: return (flags & 2) ? PFI_OK : PFI_UNSUPPORTED;
    case 18: return (flags & 4) ? PFI_OK : PFI_UNSUPPORTED;
    case 31: case 33: return (flags & 64) ? PFI_OK : PFI_UNSUPPORTED;
    case 32: case 43:
        return metadata ? PFI_CORRUPT : ((flags & 64) ? PFI_OK : PFI_UNSUPPORTED);
    case 34: case 36: case 37: case 38:
        return (flags & 128) ? PFI_OK : PFI_UNSUPPORTED;
    case 35: return metadata ? PFI_CORRUPT : ((flags & 128) ? PFI_OK : PFI_UNSUPPORTED);
    case 41: case 42: return (flags & 512) ? PFI_OK : PFI_UNSUPPORTED;
    default: return PFI_UNSUPPORTED;
    }
}
/* Exact framing for one chunk in a potentially larger remaining span. */
static int chunk_frame(const uint8_t *p, size_t n, size_t *length)
{
    size_t payload;
    if (n < 20) return PFI_CORRUPT;
    payload = r32(p + 4);
    if (payload > PFI_MAX_CHUNK || payload > n - 20) return PFI_CORRUPT;
    *length = payload + 20;
    if (crc_bytes(p, *length - 4) != r32(p + *length - 4)) return PFI_CORRUPT;
    return PFI_OK;
}
static int bootstrap(identity *id, const uint8_t *raw)
{
    uint32_t type = r32(raw); uint64_t index = r64(raw + 8);
    if ((index == 0 && type != 1) || (type == 1 && index != 0)
        || (index == 1 && type != 2) || (type == 2 && index != 1)
        || ((id->flags & 2) && index == 2 && type != 17)
        || (type == 17 && index != 2)) return PFI_CORRUPT;
    if (type == 1) return header_identity(id, raw + 16, r32(raw + 4));
    return PFI_OK;
}
static int metadata(identity *id, const pfi_limits *limits, const uint8_t *p,
                    size_t bytes, uint32_t count, uint64_t next)
{
    uint32_t i; uint64_t previous = 0; unsigned anchors = 0;
    /* Counted encoding includes the original header even in combined form. */
    if (limits->max_metadata_bytes < 16 || bytes > limits->max_metadata_bytes - 16u)
        return PFI_SPACE;
    if ((uint64_t)count > (uint64_t)(bytes / 24)) return PFI_CORRUPT;
    for (i = 0; i < count; ++i) {
        size_t len, framed; uint64_t index; uint32_t type; int rc;
        if (bytes < 4) return PFI_CORRUPT;
        len = r32(p); p += 4; bytes -= 4;
        if (len > bytes) return PFI_CORRUPT;
        rc = chunk_frame(p, len, &framed); if (rc) return rc;
        if (framed != len) return PFI_CORRUPT;
        type = r32(p); index = r64(p + 8);
        if (index >= next || (i && index <= previous)) return PFI_CORRUPT;
        rc = chunk_type(type, id->flags, 1); if (rc) return rc;
        rc = bootstrap(id, p); if (rc) return rc;
        if (index < 3) anchors |= 1u << (unsigned)index;
        previous = index; p += len; bytes -= len;
    }
    if (bytes || (next > 0 && !(anchors & 1)) || (next > 1 && !(anchors & 2))
        || ((id->flags & 2) && next > 2 && !(anchors & 4))) return PFI_CORRUPT;
    return PFI_OK;
}

static int checkpoint(identity *id, const pfi_limits *limits, const pfr_segment *seg,
                      uint64_t prefix_bytes)
{
    const uint8_t *p = seg->checkpoint, *original, *sections;
    size_t n = seg->checkpoint_bytes, fixed, remain;
    uint64_t next, bytes; uint32_t kind, section_count, i; int rc;
    if (n > limits->max_checkpoint_bytes) return PFI_SPACE;
    if (n < 68) return PFI_CORRUPT;
    if (crc_bytes(p, n - 4) != r32(p + n - 4)) return PFI_CORRUPT;
    if (!memcmp(p, "PFRCKP01", 8)) {
        kind = PFI_CHECKPOINT_PPU; fixed = 64; section_count = 3;
        if (r16(p + 8) != 1) return PFI_UNSUPPORTED;
        if (r16(p + 10) != 3 || r32(p + 12) || r32(p + 28) || r64(p + 56)
            || r64(p + 40) != (uint64_t)(n - 68)) return PFI_CORRUPT;
        next = r64(p + 16); bytes = r64(p + 32); original = NULL;
    } else if (!memcmp(p, "PFRMIX01", 8)) {
        uint32_t version, flags, families;
        kind = PFI_CHECKPOINT_COMBINED;
        if (n < 84) return PFI_CORRUPT;
        version = r16(p + 8);
        if (version == 1) { fixed = 80; section_count = 4; }
        else if (version == 2) { fixed = 96; section_count = 6; }
        else if (version == 3) { fixed = 128; section_count = 6; }
        else return PFI_UNSUPPORTED;
        if (n < fixed + 4) return PFI_CORRUPT;
        flags = r32(p + 28);
        families = 3u | ((flags & 128) ? 4u : 0u) | ((flags & 64) ? 8u : 0u);
        if ((version == 1 && (flags & 192)) || (version == 2 && !(flags & 192))
            || (version == 3 && !(flags & 128))
            || r16(p + 10) != fixed || r32(p + 12) != section_count
            || r64(p + 48) != (uint64_t)(n - fixed - 4)
            || r32(p + 56) != families || r32(p + 60)
            || !zeroes(p + 64 + 2 * section_count, (version == 3 ? 80 : fixed) - 64 - 2 * section_count))
            return PFI_CORRUPT;
        for (i = 0; i < section_count; ++i)
            if (r16(p + 64 + 2 * i) != 1) return PFI_UNSUPPORTED;
        next = r64(p + 32); bytes = r64(p + 40); original = p + 16;
        if (version == 3) {
            /* Retired-window census framing only: these bounds never make the
             * census, or the capture, semantically valid. */
            uint64_t floor = r64(p + 80), count = r64(p + 88), max_id = r64(p + 96);
            uint64_t last_frame = r64(p + 104), end = r64(p + 112);
            if (floor > next || max_id > UINT32_MAX || last_frame >= UINT64_C(1) << 63
                || r64(p + 120)) return PFI_CORRUPT;
            if (!count) {
                if (max_id || last_frame || end) return PFI_CORRUPT;
            } else if (end >= floor || count > max_id || 2 * count + 1 > end
                       || !(flags & 512)) return PFI_CORRUPT;
        }
    } else return PFI_UNSUPPORTED;
    if (next == UINT64_MAX || next != seg->first_chunk || bytes != prefix_bytes
        || bytes > UINT64_MAX - 16) return PFI_CORRUPT;
    sections = p + fixed; remain = n - fixed - 4;
    for (i = 1; i <= section_count; ++i) {
        uint32_t count; size_t len; const uint8_t *body;
        if (remain < 16) return PFI_CORRUPT;
        if (r16(sections) != i || r32(sections + 12)) return PFI_CORRUPT;
        if (r16(sections + 2) != 1) return PFI_UNSUPPORTED;
        count = r32(sections + 4); len = r32(sections + 8);
        sections += 16; remain -= 16;
        if (len > remain) return PFI_CORRUPT;
        body = sections; sections += len; remain -= len;
        if (i == 1) {
            if (kind == PFI_CHECKPOINT_PPU) {
                if (len < 16) return PFI_CORRUPT;
                original = body; body += 16; len -= 16;
                if (r32(original + 12) != r32(p + 24)) return PFI_CORRUPT;
            }
            rc = original_header(id, original, kind); if (rc) return rc;
            rc = metadata(id, limits, body, len, count, next); if (rc) return rc;
        } else if (i == 5 || i == 6) {
            uint32_t flag = i == 5 ? 128u : 64u;
            if (!(id->flags & flag)) {
                if (count || len) return PFI_CORRUPT;
            } else {
                if (count != 1) return PFI_CORRUPT;
                if (i == 5) {
                    if (len < 32 || (uint64_t)r32(body) != (uint64_t)(len - 32)
                        || r32(body + 4) || !zeroes(body + 16, 16)) return PFI_CORRUPT;
                } else {
                    if (len < 64 || r64(body + 56)
                        || UINT64_C(64) + r32(body) + UINT64_C(32) * r32(body + 4)
                           != (uint64_t)len) return PFI_CORRUPT;
                }
            }
        }
        /* Table/journal bodies, including family wrapper contents, are opaque. */
    }
    return remain ? PFI_CORRUPT : PFI_OK;
}
static int trailer(const uint8_t *p, size_t n)
{
    uint32_t count, flags, i; const uint8_t *tail; int incomplete = 0;
    if (n < 40) return PFI_CORRUPT;
    flags = r32(p); count = r32(p + 4);
    if ((uint64_t)count != (uint64_t)((n - 40) / 56) || (n - 40) % 56 || (flags & ~7u))
        return PFI_CORRUPT;
    tail = p + 8 + (size_t)count * 56;
    if (r32(tail + 12)) return PFI_CORRUPT;
    if (flags || r64(tail) || r32(tail + 8)) incomplete = 1;
    for (i = 0; i < count; ++i) {
        const uint8_t *entry = p + 8 + (size_t)i * 56;
        if (entry[4] < 1 || entry[4] > 3 || (entry[5] & ~3u) || r16(entry + 6)) return PFI_CORRUPT;
        if (entry[4] == 3 || entry[5]) incomplete = 1;
    }
    return incomplete ? PFI_INCOMPLETE : PFI_OK;
}

static int buffers_valid(const pfr_io *io, const pfi_limits *limits, const pfi_buffers *b,
                         const char *app, size_t app_bytes, const pfi_result *out)
{
    const void *storage[3]; size_t sizes[3]; unsigned i, j;
    storage[0] = b->store; sizes[0] = b->store_bytes;
    storage[1] = b->slot; sizes[1] = b->slot_bytes;
    storage[2] = b->work; sizes[2] = b->work_bytes;
    if ((uintptr_t)b->store % _Alignof(max_align_t)) return 0;
    for (i = 0; i < 3; ++i) {
        if (!range_valid(storage[i], sizes[i])
            || overlaps(storage[i], sizes[i], io, sizeof *io)
            || overlaps(storage[i], sizes[i], limits, sizeof *limits)
            || overlaps(storage[i], sizes[i], b, sizeof *b)
            || overlaps(storage[i], sizes[i], app, app_bytes)
            || overlaps(storage[i], sizes[i], out, sizeof *out)) return 0;
        for (j = 0; j < i; ++j)
            if (overlaps(storage[i], sizes[i], storage[j], sizes[j])) return 0;
    }
    return 1;
}
int pfi_inspect(const pfr_io *io, const pfi_limits *limits, const pfi_buffers *buffers,
                const char *app, size_t app_bytes, uint64_t session, pfi_result *out)
{
    pfi_requirements q; pfi_result result; pfr_io ro; pfr_view view;
    pfr_store *store; identity id; uint64_t bytes, next, off, extent;
    uint32_t i, crc = UINT32_MAX; size_t j; int rc, have_trailer = 0;
    if (!io || !io->read_at || !io->size || !limits || !buffers || !out || !app
        || !app_bytes || app_bytes > 32) return PFI_INVALID;
    for (j = 0; j < app_bytes; ++j)
        if (!((app[j] >= 'a' && app[j] <= 'z') || (app[j] >= 'A' && app[j] <= 'Z')
            || (app[j] >= '0' && app[j] <= '9') || app[j] == '_' || app[j] == '-')) return PFI_INVALID;
    if (!buffers_valid(io, limits, buffers, app, app_bytes, out)) return PFI_INVALID;
    if (buffers->store_bytes < pfr_state_bytes() || buffers->work_bytes < PFI_WORK_BYTES) return PFI_SPACE;
    rc = pfi_measure(io, limits, buffers->work, buffers->work_bytes, &q); if (rc) return rc;
    if (buffers->slot_bytes < q.layout.scratch_bytes) return PFI_SPACE;
    ro = *io; ro.write_at = NULL; ro.resize = NULL; ro.sync = NULL;
    store = buffers->store;
    rc = pfr_open(store, &ro, &q.config, buffers->slot, buffers->slot_bytes);
    if (rc != PFR_OK) return physical_error(rc);
    rc = pfr_get_view(store, &view); if (rc != PFR_OK) return physical_error(rc);
    if (view.format_version != 2) return PFI_UNSUPPORTED;
    if (view.state != PFR_SEALED || view.reason || !view.retained_segments) return PFI_INCOMPLETE;
    memset(&id, 0, sizeof id); id.app = app; id.app_bytes = app_bytes; id.session = session;
    bytes = view.evicted_payload_bytes; next = view.first_chunk;
    for (i = 0; i < view.retained_segments; ++i) {
        pfr_segment seg; const uint8_t *p; size_t remain;
        rc = pfr_get_segment(store, i, &seg); if (rc != PFR_OK) return physical_error(rc);
        if (seg.first_chunk != next) return PFI_CORRUPT;
        rc = checkpoint(&id, limits, &seg, bytes); if (rc) return rc;
        p = seg.payload; remain = seg.payload_bytes;
        while (remain) {
            size_t len; uint32_t type;
            rc = chunk_frame(p, remain, &len); if (rc) return rc;
            if (r64(p + 8) != next || next == UINT64_MAX || have_trailer) return PFI_CORRUPT;
            type = r32(p);
            rc = chunk_type(type, id.flags, 0); if (rc) return rc;
            rc = bootstrap(&id, p); if (rc) return rc;
            if (type == 6) {
                if (i + 1 != view.retained_segments || len != remain) return PFI_CORRUPT;
                rc = trailer(p + 16, len - 20); if (rc) return rc;
                have_trailer = 1;
            }
            ++next; p += len; remain -= len;
        }
        if (next != seg.next_chunk || (uint64_t)seg.payload_bytes > UINT64_MAX - bytes) return PFI_CORRUPT;
        bytes += seg.payload_bytes;
    }
    if (next != view.next_chunk || bytes != view.bound_payload_bytes) return PFI_CORRUPT;
    if (!have_trailer || !id.have_identity || next < ((id.flags & 2) ? 4u : 3u)) return PFI_INCOMPLETE;
    /* Includes both roots, spare/unreferenced slots and padding. No digest of
     * just the retained payload can identify the indexed physical file. */
    for (off = 0; off < q.layout.file_bytes; ) {
        uint64_t left = q.layout.file_bytes - off;
        size_t n = left < PFI_WORK_BYTES ? (size_t)left : PFI_WORK_BYTES;
        rc = read_exact(io, q.layout.file_bytes, off, buffers->work, n); if (rc) return rc;
        crc = crc_add(crc, buffers->work, n); off += n;
    }
    if (io->size(io->ctx, &extent)) return PFI_IO;
    if (extent != q.layout.file_bytes) return PFI_CORRUPT;
    memset(&result, 0, sizeof result);
    result.file_bytes = extent; result.generation = view.generation;
    result.first_chunk = view.first_chunk; result.next_chunk = view.next_chunk;
    result.file_crc32 = ~crc; result.checkpoint_kind = id.kind; result.original_flags = id.flags;
    result.recovered = view.recovered; result.evidence = PFI_IDENTITY_FRAMING_FINAL;
    memcpy(result.physical_session, q.config.session, 16);
    memcpy(out, &result, sizeof result); return PFI_OK;
}
