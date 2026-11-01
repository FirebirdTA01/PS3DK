#include "prof_arcs_stream.h"
#include <string.h>

static int text_length(const char *s, uint32_t *length) {
    uint32_t n=0, remaining=0, value=0, minimum=0;
    if (!s) return 0;
    for (; n<=512; ++n) {
        unsigned char c=(unsigned char)s[n];
        if (!c) { if (remaining) return 0; *length=n; return 1; }
        if (remaining) {
            if ((c&0xc0)!=0x80) return 0;
            value=(value<<6)|(c&63); --remaining;
            if (!remaining && (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))) return 0;
        } else if (c>=0x80) {
            if (c>=0xc2 && c<=0xdf) { remaining=1; value=c&31; minimum=0x80; }
            else if (c>=0xe0 && c<=0xef) { remaining=2; value=c&15; minimum=0x800; }
            else if (c>=0xf0 && c<=0xf4) { remaining=3; value=c&7; minimum=0x10000; }
            else return 0;
        }
    }
    return 0;
}
static void number(unsigned char *p, uint64_t v, unsigned n) {
    for (unsigned i=0; i<n; ++i) p[i]=(unsigned char)(v>>(8*i));
}
static uint32_t crc32_extend(uint32_t crc, const unsigned char *p, size_t n) {
    crc=~crc;
    for (size_t i=0; i<n; ++i) {
        crc^=p[i];
        for (unsigned bit=0; bit<8; ++bit) crc=(crc>>1)^(UINT32_C(0xedb88320)&(0u-(crc&1)));
    }
    return ~crc;
}
/* One callback per complete chunk. The budget and sequence were preflighted
 * for the WHOLE view. A callback failure may have written a partial chunk. */
static int chunk(struct ps3dk_prof_arc_stream *s, unsigned char *b, uint32_t kind, uint32_t len) {
    number(b,kind,4); number(b+4,len,4); number(b+8,s->next_sequence,8);
    number(b+16+len,crc32_extend(0,b,16+len),4);
    if (s->write_all(s->ctx,b,20+len)!=0) {
        s->error=PS3DK_PROF_STREAM_IO;
        return s->error;
    }
    s->body_crc32=crc32_extend(s->body_crc32,b,20+len);
    s->body_bytes+=20+len;
    ++s->next_sequence;
    return 0;
}
static int add(uint64_t *sum, uint64_t value) {
    if (value>UINT64_MAX-*sum) return 0;
    *sum+=value; return 1;
}
static int valid_view(const struct ps3dk_prof_arc_stream *s, const struct ps3dk_prof_arc_view *v, uint32_t *name_len) {
    if (!v) return 0;
    const struct ps3prof_arc_thread *t=&v->thread;
    if (!v->generation || !t->incarnation || t->slot>=s->max_threads || t->state<1 || t->state>3 ||
        !text_length(t->name,name_len) || t->row_count>s->max_arcs_per_thread || (t->row_count && !t->rows)) return 0;
    const struct ps3dk_prof_stream_slot *old=&s->slots[t->slot];
    if (old->state && (old->state!=1 || v->generation<=old->generation || t->incarnation<=old->incarnation)) return 0;
    if (t->state!=1 && s->final_count>=s->max_threads) return 0;
    if (t->state==3)
        return t->row_count==0 && t->limit_flags==2 && t->attempted==UINT64_MAX && t->recorded==UINT64_MAX &&
               t->refused==UINT64_MAX && t->lost==UINT64_MAX;
    if (t->limit_flags>1) return 0;
    uint64_t sum=0;
    for (uint32_t i=0; i<t->row_count; ++i) {
        const struct ps3prof_arc_row *r=&t->rows[i];
        if (!r->count || !r->callee_hook_pc || r->callee_hook_pc%4 || r->flags>1 ||
            (r->flags ? r->caller_return_pc!=0 : (!r->caller_return_pc || r->caller_return_pc%4)) ||
            !add(&sum,r->count)) return 0;
    }
    /* Keys are unique in the runtime collector. This private streaming path
     * does not add an O(n^2) duplicate scan or a second row index; the final
     * production reader independently checks key and incarnation uniqueness. */
    return sum==t->recorded && add(&sum,t->refused) && add(&sum,t->lost) && sum==t->attempted;
}
int ps3dk_prof__stream_init(struct ps3dk_prof_arc_stream *s,
                          struct ps3dk_prof_stream_slot *slots, struct ps3dk_prof_stream_final *finals,
                          uint32_t threads, uint32_t arcs, uint64_t bytes, ps3prof_arcs_sink sink, void *ctx) {
    if (!s) return PS3DK_PROF_STREAM_INVALID;
    if (s->initialized) return PS3DK_PROF_STREAM_STATE;
    if (!slots || !finals || !sink || !threads || threads>1024 || !arcs || arcs>65536)
        return PS3DK_PROF_STREAM_INVALID;
    s->slots=slots; s->finals=finals; s->max_threads=threads; s->max_arcs_per_thread=arcs;
    s->max_body_bytes=bytes; s->write_all=sink; s->ctx=ctx; s->next_sequence=2; s->initialized=1;
    return 0;
}
int ps3dk_prof__stream_accept(struct ps3dk_prof_arc_stream *s, const struct ps3dk_prof_arc_view *v) {
    uint32_t name_len;
    unsigned char b[560]; /* largest chunk: 20 framing + 22 identity + 512 name */
    if (!s || !s->initialized || s->finished || s->busy) return PS3DK_PROF_STREAM_STATE;
    if (s->error) return s->error;
    if (!valid_view(s,v,&name_len)) return s->error=PS3DK_PROF_STREAM_INVALID;
    const struct ps3prof_arc_thread *t=&v->thread;
    uint64_t bytes=42+name_len+(uint64_t)t->row_count*60+(t->state==1?64:0);
    uint64_t chunks=1+(uint64_t)t->row_count+(t->state==1);
    /* Leave one sequence increment for the final TRAILER; no wrap, including
     * when a synthetic test starts near the representable boundary. */
    if (s->body_bytes>s->max_body_bytes || bytes>s->max_body_bytes-s->body_bytes ||
        s->next_sequence>=UINT64_MAX || chunks>UINT64_MAX-1-s->next_sequence)
        return s->error=PS3DK_PROF_STREAM_LIMIT;
    s->busy=1;
    number(b+16,1,4); number(b+20,t->incarnation,4); number(b+24,t->slot,4);
    number(b+28,t->os_thread_id,8); number(b+36,name_len,2); memcpy(b+38,t->name,name_len);
    if (chunk(s,b,4,22+name_len)) goto done;
    for (uint32_t i=0; i<t->row_count; ++i) {
        const struct ps3prof_arc_row *r=&t->rows[i];
        number(b+16,t->incarnation,4); number(b+20,1,4);
        number(b+24,r->caller_return_pc,8); number(b+32,r->callee_hook_pc,8);
        number(b+40,r->count,8); number(b+48,r->flags,4); number(b+52,0,4);
        if (chunk(s,b,8,40)) goto done;
    }
    if (t->state==1) {
        number(b+16,1,4); number(b+20,t->incarnation,4); b[24]=t->state; b[25]=t->limit_flags;
        number(b+26,0,2); number(b+28,t->attempted,8); number(b+36,t->recorded,8);
        number(b+44,t->refused,8); number(b+52,t->lost,8);
        if (chunk(s,b,7,44)) goto done;
    } else {
        s->finals[s->final_count++]=(struct ps3dk_prof_stream_final){
            .incarnation=t->incarnation,.state=t->state,.limit_flags=t->limit_flags,
            .attempted=t->attempted,.recorded=t->recorded,.refused=t->refused,.lost=t->lost};
    }
    s->slots[t->slot]=(struct ps3dk_prof_stream_slot){
        .generation=v->generation,.incarnation=t->incarnation,.state=t->state};
    if (t->state==3) s->end_flags|=1;
    if (t->limit_flags&1) s->end_flags|=2;
done:
    s->busy=0;
    return s->error;
}
int ps3dk_prof__stream_finish(struct ps3dk_prof_arc_stream *s, uint64_t n) {
    if (!s || !s->initialized || s->finished || s->busy) return PS3DK_PROF_STREAM_STATE;
    if (s->error) return s->error;
    s->unregistered_refusals=n; s->finished=1;
    return 0;
}

int ps3dk_prof__stream_replay(struct ps3dk_prof_arc_stream *s,
                            ps3dk_prof_stream_read read_some, void *read_ctx,
                            ps3prof_arcs_sink write_all, void *write_ctx,
                            void *scratch, size_t scratch_bytes) {
    if (!s || !s->initialized || !s->finished || s->busy || s->replay_attempted)
        return PS3DK_PROF_STREAM_STATE;
    if (s->error) return s->error;
    if (!read_some || !write_all || !scratch || !scratch_bytes || scratch_bytes>65536)
        return PS3DK_PROF_STREAM_INVALID;
    /* No retries, including from inside either callback or after failure.
     * The finished stream and its backing storage are immutable to callbacks. */
    s->replay_attempted=1;
    uint64_t remaining=s->body_bytes;
    uint32_t crc=0;
    while (remaining) {
        size_t n=remaining<scratch_bytes?(size_t)remaining:scratch_bytes;
        ptrdiff_t got=read_some(read_ctx,scratch,n);
        if (got<=0 || (size_t)got>n) return s->error=PS3DK_PROF_STREAM_IO;
        crc=crc32_extend(crc,scratch,(size_t)got);
        if (write_all(write_ctx,scratch,(size_t)got)!=0) return s->error=PS3DK_PROF_STREAM_IO;
        remaining-=(size_t)got;
    }
    /* Even an empty spool must be empty on read. No trailer may follow unless
     * both this exact EOF check and the whole-body CRC check succeed. */
    if (read_some(read_ctx,scratch,1)!=0 || crc!=s->body_crc32)
        return s->error=PS3DK_PROF_STREAM_IO;
    return 0;
}
