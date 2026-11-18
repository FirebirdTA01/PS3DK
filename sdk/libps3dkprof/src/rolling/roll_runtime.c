#include "../core/prof_internal.h"
#ifdef P_ROLLING_RUNTIME
#include <string.h>

/* All runtime state is a single region in the existing core allocation. The
 * same checked partition is used for sizing and binding; no post-READY alloc. */
static void *part(uint8_t *base, size_t *n, size_t bytes)
{
    size_t at;
    if (*n > SIZE_MAX - 127) return NULL;
    at = (*n + 127) & ~(size_t)127;
    if (bytes > SIZE_MAX - at) return NULL;
    *n = at + bytes;
    return base ? base + at : (void *)1;
}

static int layout(const p_roll_options *o, const p_roll_selection *selection, uint8_t *base,
                  p_roll_runtime *r, size_t *bytes)
{
    pfr_layout disk;
    pfp_layout enc;
    size_t n = 0;
    unsigned i;
    void *v;
    const pfm_config *m = selection->kind == P_ROLL_COMBINED ? &selection->limits.combined :
                         selection->kind == P_ROLL_SCHEMA2 ? &selection->limits.schema2.shared : NULL;
    if (m) {
#ifdef PS3DK_PROF_WITH_SPU
        pfm_layout combined;
        if (selection->kind == P_ROLL_SCHEMA2) {
#ifdef P_ROLLING_SCHEMA2
            if (pfm2_plan(&selection->limits.schema2, &combined)) return PS3DK_PROF_EINVAL;
#else
            return PS3DK_PROF_ENOTSUP;
#endif
        } else if (pfm_plan(m, &combined)) return PS3DK_PROF_EINVAL;
        memset(&enc, 0, sizeof enc);
        enc.context_bytes = combined.context_bytes;
        enc.arena_bytes = combined.arena_bytes;
        enc.arena_alignment = combined.arena_alignment;
        enc.checkpoint_bound = combined.checkpoint_bound;
#else
        return PS3DK_PROF_ENOTSUP;
#endif
    } else if (pfp_plan(&o->ppu, &enc)) return PS3DK_PROF_EINVAL;
    if (pfr_plan(&o->store, &disk) ||
        o->store.payload_bytes < P_CHUNK_CAP + 20u ||
        o->store.checkpoint_bytes < (m ? m->checkpoint_bytes : o->ppu.checkpoint_bytes) ||
        !o->max_state_bytes || o->max_state_bytes > 64u * 1024u * 1024u)
        return PS3DK_PROF_EINVAL;
#define REGION(field, count) do { v = part(base, &n, (count)); if (!v) return PS3DK_PROF_EINVAL; if (r) r->field = v; } while (0)
    if (!part(base, &n, sizeof(p_roll_runtime))) return PS3DK_PROF_EINVAL;
    REGION(store, pfr_state_bytes());
    REGION(output, pfo_state_bytes());
    REGION(buffers.encoder_ctx, enc.context_bytes);
    for (i = 0; i < 2; ++i) { REGION(buffers.arenas[i], enc.arena_bytes); }
    for (i = 0; i < 3; ++i) { REGION(buffers.checkpoints[i], o->store.checkpoint_bytes); }
    REGION(initial, o->store.checkpoint_bytes);
    REGION(buffers.payload, o->store.payload_bytes);
    REGION(scratch, disk.scratch_bytes);
#undef REGION
    if ((uint64_t)n > o->max_state_bytes) return PS3DK_PROF_EINVAL;
    if (r) {
        r->disk = disk; r->encoder = enc;
        r->buffers.arena_bytes = enc.arena_bytes;
        r->buffers.checkpoint_capacity = o->store.checkpoint_bytes;
        r->buffers.payload_capacity = o->store.payload_bytes;
        r->buffers.prepare = pfp_prepare;
#ifdef PS3DK_PROF_WITH_SPU
        if (m) r->buffers.prepare = pfm_prepare;
#endif
#ifdef P_ROLLING_SCHEMA2
        if (selection->kind == P_ROLL_SCHEMA2) r->buffers.prepare = pfm2_prepare;
#endif
    }
    *bytes = n;
    return 0;
}

int p_roll_plan(const p_roll_options *o, const p_roll_selection *selection, size_t *bytes)
{
    if (!o || !bytes || o->size != sizeof *o || o->version != 1 ||
        o->reserved || o->enabled > 1) return PS3DK_PROF_EINVAL;
    if (selection && selection->kind != P_ROLL_PPU &&
             (o->ppu.incarnations || o->ppu.descriptors || o->ppu.stack_rows ||
              o->ppu.frame_identities || o->ppu.metadata_bytes || o->ppu.checkpoint_bytes))
        return PS3DK_PROF_EINVAL;
    if (!o->enabled) { *bytes = 0; return 0; }
    if (!selection || selection->kind < P_ROLL_PPU || selection->kind > P_ROLL_SCHEMA2)
        return PS3DK_PROF_EINVAL;
    return layout(o, selection, NULL, NULL, bytes);
}

void p_roll_bind(prof *p, void *base, const p_roll_options *o, const p_roll_selection *selection, size_t total)
{
    p_roll_runtime *r = base;
    size_t bytes;
    memset(r, 0, sizeof *r);
    r->options = *o;
    r->selection = *selection;
    (void)layout(o, selection, base, r, &bytes); /* checked before core allocation */
    r->record.size = sizeof r->record; r->record.version = 1;
    r->record.state_bytes = total; r->record.file_bytes = r->disk.file_bytes;
    p->rolling = r;
}

static int observe(prof *p, int rc)
{
    p_roll_runtime *r = p->rolling;
    pfo_view out;
    pfr_view store;
    if (!pfo_get_view(r->output, &out)) {
        r->record.accepted_next_chunk = out.accepted_next_chunk;
        r->record.accepted_payload_bytes = out.accepted_payload_bytes;
        r->record.committed_next_chunk = out.committed_next_chunk;
        r->record.committed_payload_bytes = out.committed_payload_bytes;
        r->record.reason = out.reason;
    }
    if (!pfr_get_view(r->store, &store)) {
        r->record.revision = store.generation;
        r->record.state = store.state;
    }
    if (rc == PFO_RESOURCE) {
        r->result = P_ROLL_RESOURCE;
    } else if (rc != PFR_OK) {
        /* A later flush of the poisoned output reports PFR_FAILED. Preserve
         * the original refusal, just as the sink's first_error does. */
        if (!r->record.io_result) r->record.io_result = rc;
        r->result = PS3DK_PROF_ESINKFAIL;
        p->snk.failed = 1;
        if (!p->snk.first_error) p->snk.first_error = rc;
    }
    return r->result;
}

int p_roll_start(prof *p, const uint8_t head[16])
{
    p_roll_runtime *r = p->rolling;
    size_t initial;
    int rc;
#ifdef PS3DK_PROF_ROLLING_PUBLIC
    /* Bind physical slots to this exclusive capture, not an app-supplied ID.
     * This is an accidental-mix namespace, not cryptographic authentication.
     * The original HEADER/CLOCKS identity is still checked independently. */
    {
        uint8_t empty[16]={0};
        if(!memcmp(r->options.store.session,empty,sizeof empty)) {
            uint64_t path=p_fnv(p->fnv_build,p->snk.path,strlen(p->snk.path));
            p_le64(r->options.store.session,p->tb_at_init);
            p_le64(r->options.store.session+8,path);
        }
    }
#endif
    r->record.original_flags = (uint32_t)head[12] | (uint32_t)head[13] << 8 |
                              (uint32_t)head[14] << 16 | (uint32_t)head[15] << 24;
#ifdef PS3DK_PROF_WITH_SPU
    if (r->selection.kind == P_ROLL_COMBINED)
        rc = pfm_init(r->buffers.encoder_ctx, r->encoder.context_bytes, &r->selection.limits.combined,
                     head, r->buffers.arenas[0], r->encoder.arena_bytes,
                     r->initial, r->options.store.checkpoint_bytes, &initial);
    else
#endif
#ifdef P_ROLLING_SCHEMA2
    if (r->selection.kind == P_ROLL_SCHEMA2)
        rc = pfm2_init(r->buffers.encoder_ctx, r->encoder.context_bytes, &r->selection.limits.schema2,
                      head, r->buffers.arenas[0], r->encoder.arena_bytes,
                      r->initial, r->options.store.checkpoint_bytes, &initial);
    else
#endif
    rc = pfp_init(r->buffers.encoder_ctx, r->encoder.context_bytes, &r->options.ppu,
                 head, r->buffers.arenas[0], r->encoder.arena_bytes,
                 r->initial, r->options.store.checkpoint_bytes, &initial);
    if (rc) return PS3DK_PROF_EINVAL;
#ifdef P_ROLLING_SCHEMA2
    if (r->selection.kind == P_ROLL_SCHEMA2 &&
        pfm2_retention(r->buffers.encoder_ctx,r->buffers.arenas[0],r->options.store.retained_slots,
                      r->initial,r->options.store.checkpoint_bytes,&initial))
        return PS3DK_PROF_EINVAL;
#endif
    if (pfr_file_bind(&r->file, &r->io, p->snk.fd, r->disk.file_bytes) ||
        pfr_format_v2(r->store, &r->io, &r->options.store, r->scratch, r->disk.scratch_bytes) ||
        pfo_bind(r->output, r->store, &r->buffers, r->initial, initial))
        return PS3DK_PROF_ESINKFAIL;
#ifdef P_ROLLING_SCHEMA2
    if (r->selection.kind == P_ROLL_SCHEMA2 && pfo_support(r->output,pfm2_pressure,pfm2_project))
        return PS3DK_PROF_EINVAL;
#endif
    return observe(p, 0);
}

int p_roll_accept(prof *p, const void *chunk, size_t bytes)
{
    if (p->rolling->result) return p->rolling->result;
    return observe(p, pfo_accept(p->rolling->output, chunk, bytes));
}
int p_roll_flush(prof *p)
{
    if (!p->rolling) return 0;
    if (p->rolling->result) return p->rolling->result;
    return observe(p, pfo_flush(p->rolling->output));
}
int p_roll_finish(prof *p)
{
    int rc;
    if (!p->rolling) return 0;
    /* Cache the acknowledged tail root before the separate terminal write.
     * A failed terminal sync poisons the store, so its live view is no longer
     * readable even though this preceding commit completed successfully. */
    rc = pfo_flush(p->rolling->output);
    (void)observe(p, rc);
    if (rc != PFR_OK) return p->rolling->result;
    return observe(p, pfo_finish(p->rolling->output));
}
void p_roll_publish(prof *p, int result)
{
    p_roll_runtime *r = p->rolling;
    r->record.close_result = p->snk.close_failed ? PS3DK_PROF_ECLOSE : 0;
    r->record.join_result = p->join_failed ? PS3DK_PROF_ETHREAD : 0;
    r->record.result = result;
}
#endif
