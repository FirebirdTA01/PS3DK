/* Binary kind=1 serializer. Fixed-width little-endian fields in both ABIs. */
#include "prof_arcs_writer.h"

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
static int add64(uint64_t *sum, uint64_t value) {
    if (UINT64_MAX-*sum<value) return 0;
    *sum+=value; return 1;
}
static int validate(const struct ps3prof_arc_snapshot *s) {
    uint32_t n, i, j, k, text_size;
    const char *strings[5];
    if (!s || s->abi<1 || s->abi>2 || s->mode<1 || s->mode>2 || s->snapshot_flags>1 || !s->session_id || !s->snapshot_id ||
        !s->freq_hz || !s->max_threads || s->max_threads>1024 || !s->max_arcs_per_thread || s->max_arcs_per_thread>65536 ||
        s->build_id_len>512 || (s->build_id_len && !s->build_id) || (s->thread_count && !s->threads) ||
        ((s->sink_error_count==0)!=(s->first_sink_error==0))) return 0;
    if (s->mode==1 ? (s->timing_prefix_bytes!=0 || s->timing_chunk_seq!=UINT64_MAX || s->timing_prefix_crc32!=0) :
        (s->timing_prefix_bytes<16 || s->timing_chunk_seq==UINT64_MAX)) return 0;
    strings[0]=s->app; strings[1]=s->sdk_version; strings[2]=s->compiler_identity; strings[3]=s->flags; strings[4]=s->workload_tag;
    for (n=0;n<5;++n) if (!text_length(strings[n],&text_size)) return 0;
    for (i=0;i<s->thread_count;++i) {
        const struct ps3prof_arc_thread *t=&s->threads[i]; uint64_t sum=0; int sum_fits=1;
        if (!t->incarnation || t->slot>=s->max_threads || !text_length(t->name,&text_size) || t->row_count>s->max_arcs_per_thread ||
            (t->row_count && !t->rows) || t->state<1 || t->state>3) return 0;
        for (j=0;j<i;++j) if (s->threads[j].incarnation==t->incarnation || (s->threads[j].slot==t->slot && s->threads[j].state!=1)) return 0;
        for (j=0;j<t->row_count;++j) {
            const struct ps3prof_arc_row *r=&t->rows[j];
            if (!r->count || !r->callee_hook_pc || r->callee_hook_pc%4 || r->flags>1 ||
                (r->flags ? r->caller_return_pc!=0 : (!r->caller_return_pc || r->caller_return_pc%4))) return 0;
            for (k=0;k<j;++k) if (t->rows[k].caller_return_pc==r->caller_return_pc && t->rows[k].callee_hook_pc==r->callee_hook_pc) return 0;
            if (!add64(&sum,r->count)) sum_fits=0;
        }
        if (t->state==3) {
            if (t->limit_flags!=2 || t->attempted!=UINT64_MAX || t->recorded!=UINT64_MAX || t->refused!=UINT64_MAX || t->lost!=UINT64_MAX) return 0;
        } else {
            if (t->limit_flags>1 || !sum_fits || sum!=t->recorded || !add64(&sum,t->refused) || !add64(&sum,t->lost) || sum!=t->attempted) return 0;
        }
    }
    return 1;
}
struct writer { ps3prof_arcs_sink sink; void *ctx; uint64_t seq; uint32_t crc; int failed, in_chunk; };
static void put(struct writer *w, const void *data, size_t size) {
    const unsigned char *p=data; size_t i; unsigned bit;
    if (w->failed || !size) return;
    if (w->sink(w->ctx,data,size)!=0) { w->failed=1; return; }
    if (w->in_chunk) for (i=0;i<size;++i) {
        w->crc^=p[i];
        for (bit=0;bit<8;++bit) w->crc=(w->crc>>1)^(UINT32_C(0xedb88320)&(0u-(w->crc&1)));
    }
}
static void number(struct writer *w, uint64_t v, unsigned size) {
    unsigned char b[8]; unsigned i; for(i=0;i<size;++i) b[i]=(unsigned char)(v>>(i*8)); put(w,b,size);
}
static void string(struct writer *w, const char *s) {
    uint32_t n=0; (void)text_length(s,&n); number(w,n,2); put(w,s,n);
}
static void begin(struct writer *w, uint32_t kind, uint32_t len) {
    w->crc=UINT32_MAX; w->in_chunk=1; number(w,kind,4); number(w,len,4); number(w,w->seq++,8);
}
static void finish(struct writer *w) { uint32_t crc=~w->crc; w->in_chunk=0; number(w,crc,4); }
static void final_entry(struct writer *w, const struct ps3prof_arc_thread *t) {
    number(w,t->incarnation,4); number(w,t->state,1); number(w,t->limit_flags,1); number(w,0,2);
    number(w,t->attempted,8); number(w,t->recorded,8); number(w,t->refused,8); number(w,t->lost,8);
}
int ps3prof_arcs_write(const struct ps3prof_arc_snapshot *s, ps3prof_arcs_sink sink, void *ctx) {
    struct writer w={sink,ctx,0,0,0,0}; uint32_t length=12+48, n, i, j, active=0, end_flags=0;
    const char *strings[5];
    if (!sink || !validate(s)) return PS3PROF_ARCS_INVALID;
    strings[0]=s->app;strings[1]=s->sdk_version;strings[2]=s->compiler_identity;strings[3]=s->flags;strings[4]=s->workload_tag;
    length+=s->build_id_len;
    for (i=0;i<5;++i) { (void)text_length(strings[i],&n); length+=n; }
    put(&w,"PS3DKPRF",8);number(&w,1,2);number(&w,0,2);number(&w,1,4);
    begin(&w,1,length);string(&w,s->app);number(&w,s->build_id_len,2);put(&w,s->build_id,s->build_id_len);
    for (i=1;i<5;++i) string(&w,strings[i]);
    number(&w,s->abi,1);number(&w,s->mode,1);number(&w,s->snapshot_flags,2);
    number(&w,s->session_id,8);number(&w,s->snapshot_id,8);number(&w,s->timing_prefix_bytes,8);number(&w,s->timing_chunk_seq,8);
    number(&w,s->timing_prefix_crc32,4);number(&w,s->max_threads,4);number(&w,s->max_arcs_per_thread,4);finish(&w);
    begin(&w,2,28);number(&w,1,4);number(&w,1,4);number(&w,0,4);number(&w,s->freq_hz,8);number(&w,s->tb_at_init,8);finish(&w);
    for (i=0;i<s->thread_count && !w.failed;++i) {
        const struct ps3prof_arc_thread *t=&s->threads[i];
        (void)text_length(t->name,&n);begin(&w,4,22+n);number(&w,1,4);number(&w,t->incarnation,4);number(&w,t->slot,4);number(&w,t->os_thread_id,8);string(&w,t->name);finish(&w);
        for (j=0;j<t->row_count && !w.failed;++j) {
            const struct ps3prof_arc_row *r=&t->rows[j];
            begin(&w,8,40);number(&w,t->incarnation,4);number(&w,1,4);number(&w,r->caller_return_pc,8);number(&w,r->callee_hook_pc,8);number(&w,r->count,8);number(&w,r->flags,4);number(&w,0,4);finish(&w);
        }
        if (t->state==1) { begin(&w,7,44);number(&w,1,4);final_entry(&w,t);finish(&w); } else ++active;
        if (t->state==3) end_flags|=1;
        if (t->limit_flags&1) end_flags|=2;
    }
    if (s->sink_error_count) end_flags|=4;
    begin(&w,6,20+40*active);number(&w,end_flags,4);number(&w,s->sink_error_count,8);number(&w,(uint32_t)s->first_sink_error,4);number(&w,active,4);
    for (i=0;i<s->thread_count && !w.failed;++i) if (s->threads[i].state!=1) final_entry(&w,&s->threads[i]);
    finish(&w);
    return w.failed ? PS3PROF_ARCS_SINK : PS3PROF_ARCS_OK;
}
