#include "prof_arcs_export.h"
#include "prof_finalization.h"

/* Incremental wire writer: bounded scalars/text, never a session-sized buffer.
 * Kept separate from the one-shot snapshot validator: these identities come
 * exclusively from the runtime and its already-validated immutable strings. */
struct writer {
    struct ps3dk_prof_arc_output output;
    uint32_t crc;
    int in_chunk, error;
};
static void put(struct writer *w, const void *data, size_t n) {
    if (w->error || !n) return;
    if (w->output.write_all(w->output.ctx,data,n)) { w->error=PS3DK_PROF_STREAM_IO; return; }
    if (w->in_chunk) {
        const unsigned char *p=data;
        for (size_t i=0;i<n;++i) {
            w->crc^=p[i];
            for (unsigned bit=0;bit<8;++bit)
                w->crc=(w->crc>>1)^(UINT32_C(0xedb88320)&(0u-(w->crc&1)));
        }
    }
}
static void number(struct writer *w, uint64_t v, unsigned n) {
    unsigned char b[8];
    for (unsigned i=0;i<n;++i) b[i]=(unsigned char)(v>>(8*i));
    put(w,b,n);
}
static void begin(struct writer *w, uint32_t kind, uint32_t len, uint64_t seq) {
    w->crc=UINT32_MAX; w->in_chunk=1;
    number(w,kind,4); number(w,len,4); number(w,seq,8);
}
static void end(struct writer *w) {
    uint32_t crc=~w->crc; w->in_chunk=0; number(w,crc,4);
}
int ps3dk_prof__stream_export(struct ps3dk_prof_arc_stream *s, uint64_t snapshot_id,
                            ps3dk_prof_stream_read read_some, void *read_ctx,
                            const struct ps3dk_prof_arc_output *output,
                            void *scratch, size_t scratch_bytes) {
    if (!s || !s->initialized || !s->finished || s->busy || s->replay_attempted || s->export_attempted)
        return PS3DK_PROF_STREAM_STATE;
    if (s->error) return s->error;
    if (!snapshot_id || !read_some || !output || !output->write_all || !output->flush || !output->close ||
        !scratch || !scratch_bytes || scratch_bytes>65536) return PS3DK_PROF_STREAM_INVALID;
    const struct ps3dk_prof_finalization *f=ps3dk_prof__finalization();
    /* Only these outcome flags make accepted_bytes the completed WHOLE file.
     * A shutdown data-quality error alone is not an I/O-failure decision. */
    if (!f || !f->io_finalized || !f->trailer_written || f->join_failed)
        return PS3DK_PROF_STREAM_TIMING;
    if (f->accepted_bytes<76 || f->completed_chunks<3 || f->last_chunk_sequence!=f->completed_chunks-1 ||
        !f->session_id || !f->freq_hz || f->abi<1 || f->abi>2 ||
        s->unregistered_refusals!=f->unregistered_refusals) return PS3DK_PROF_STREAM_TIMING;
    const struct ps3dk_prof_final_text texts[]={f->app,f->build_id,f->sdk_version,f->compiler_identity,f->flags,f->workload_tag};
    uint32_t header_length=60;
    for (unsigned i=0;i<6;++i) {
        if (texts[i].length>512 || (texts[i].length && !texts[i].data)) return PS3DK_PROF_STREAM_TIMING;
        header_length+=texts[i].length;
    }
    if (!s->max_threads || s->max_threads>1024 || !s->max_arcs_per_thread || s->max_arcs_per_thread>65536 ||
        s->final_count>s->max_threads || s->end_flags>3 || !s->finals) return PS3DK_PROF_STREAM_INVALID;
    uint64_t overhead=16+20+(uint64_t)header_length+48+40+(uint64_t)s->final_count*40;
    if (s->next_sequence<2 || s->next_sequence>=UINT64_MAX || s->body_bytes>UINT64_MAX-overhead)
        return PS3DK_PROF_STREAM_LIMIT;

    /* Admission transfers output ownership. From here every path closes once,
     * even if no byte was accepted. Input ownership always stays with caller. */
    s->export_attempted=1; s->busy=1;
    struct writer w={.output=*output};
    put(&w,"PS3DKPRF",8); number(&w,1,2); number(&w,0,2); number(&w,1,4);
    begin(&w,1,header_length,0);
    for (unsigned i=0;i<6;++i) { number(&w,texts[i].length,2); put(&w,texts[i].data,texts[i].length); }
    number(&w,f->abi,1); number(&w,2,1); number(&w,1,2);
    number(&w,f->session_id,8); number(&w,snapshot_id,8); number(&w,f->accepted_bytes,8);
    number(&w,f->last_chunk_sequence,8); number(&w,f->prefix_crc32,4);
    number(&w,s->max_threads,4); number(&w,s->max_arcs_per_thread,4); end(&w);
    begin(&w,2,28,1); number(&w,1,4); number(&w,1,4); number(&w,0,4);
    number(&w,f->freq_hz,8); number(&w,f->tb_at_init,8); end(&w);
    if (!w.error) {
        s->busy=0; /* replay latches its own attempt before either callback */
        w.error=ps3dk_prof__stream_replay(s,read_some,read_ctx,w.output.write_all,w.output.ctx,scratch,scratch_bytes);
        s->busy=1;
    }
    if (!w.error) {
        begin(&w,6,20+40*s->final_count,s->next_sequence);
        number(&w,s->end_flags,4); number(&w,0,8); number(&w,0,4); number(&w,s->final_count,4);
        for (uint32_t i=0;i<s->final_count && !w.error;++i) {
            const struct ps3dk_prof_stream_final *t=&s->finals[i];
            number(&w,t->incarnation,4); number(&w,t->state,1); number(&w,t->limit_flags,1); number(&w,0,2);
            number(&w,t->attempted,8); number(&w,t->recorded,8); number(&w,t->refused,8); number(&w,t->lost,8);
        }
        end(&w);
    }
    if (!w.error && w.output.flush(w.output.ctx)) w.error=PS3DK_PROF_STREAM_IO;
    if (w.output.close(w.output.ctx)) w.error=PS3DK_PROF_STREAM_IO;
    if (w.error) s->error=PS3DK_PROF_STREAM_IO;
    s->busy=0;
    return s->error;
}
