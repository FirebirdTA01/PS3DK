/* Accepted RAM state and committed physical state have separate frontiers.
 * Semantic checkpoint construction belongs to the supplied trusted encoder. */
#include "roll_output.h"
#include <string.h>

#define READY UINT32_C(0x50464f31)
#define MAX_CHUNK 1048576u
#define TRAILER 6u
struct pfo_output {
    uint32_t ready;
    pfr_store *store;
    pfo_buffers b;
    pfo_view v;
    unsigned arena;
    size_t start_bytes, accepted_bytes;
    pfo_pressure pressure;
    pfo_project project;
};
typedef struct { const void *p; size_t n; } area;
static int valid(area a) { return a.p && a.n && a.n <= UINTPTR_MAX - (uintptr_t)a.p; }
static int overlap(area a, area b) {
    uintptr_t x=(uintptr_t)a.p,y=(uintptr_t)b.p;
    return x<=y ? y-x<a.n : x-y<b.n;
}
static void areas(const pfo_output *o,pfr_store *s,const pfo_buffers *b,area a[8]) {
    unsigned i;
    a[0].p=o;a[0].n=sizeof *o;a[1].p=s;a[1].n=pfr_state_bytes();
    for(i=0;i<2;++i){a[2+i].p=b->arenas[i];a[2+i].n=b->arena_bytes;}
    for(i=0;i<3;++i){a[4+i].p=b->checkpoints[i];a[4+i].n=b->checkpoint_capacity;}
    a[7].p=b->payload;a[7].n=b->payload_capacity;
}
static int separate(const pfo_output *o,const void *p,size_t n) {
    area a[8],input;unsigned i;input.p=p;input.n=n;
    if(!valid(input))return 0;
    areas(o,o->store,&o->b,a);
    for(i=0;i<8;++i)if(overlap(input,a[i]))return 0;
    return 1;
}
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const uint8_t *p){return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32;}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;unsigned i;while(n--){c^=*p++;for(i=0;i<8;++i)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;}
static int poison(pfo_output *o,int rc){o->v.phase=PFO_POISONED;o->v.failure=rc;return rc;}
static int stopping(pfo_output *o,uint32_t reason){o->v.phase=PFO_STOPPING;o->v.reason=reason;return PFO_RESOURCE;}
static int usable(const pfo_output *o){
    if(!o||o->ready!=READY||o->v.phase==PFO_POISONED)return PFR_FAILED;
    if(o->v.phase==PFO_FINISHED)return PFR_END;
    return PFR_OK;
}
size_t pfo_state_bytes(void){return sizeof(pfo_output);}
int pfo_bind(pfo_output *o,pfr_store *s,const pfo_buffers *b,const void *initial,size_t initial_bytes){
    pfr_view v;pfr_config cfg;area a[8],input;unsigned i,j;
    if(!o||!s||!b||!b->prepare||!initial||!initial_bytes||initial_bytes>b->checkpoint_capacity
       ||b->payload_capacity<20||!b->arena_bytes)return PFR_INVALID;
    if(pfr_get_view(s,&v)!=PFR_OK||pfr_get_config(s,&cfg)!=PFR_OK)return PFR_FAILED;
    if(v.format_version!=2||v.state!=PFR_OPEN||v.generation!=1||v.next_chunk||v.next_segment
       ||b->checkpoint_capacity>cfg.checkpoint_bytes||b->payload_capacity>cfg.payload_bytes)return PFR_INVALID;
    areas(o,s,b,a);input.p=initial;input.n=initial_bytes;
    if(!valid(input))return PFR_INVALID;
    for(i=0;i<8;++i){
        if(!valid(a[i])||overlap(a[i],input))return PFR_INVALID;
        for(j=0;j<i;++j)if(overlap(a[i],a[j]))return PFR_INVALID;
    }
    memset(o,0,sizeof *o);o->store=s;o->b=*b;o->ready=READY;o->v.phase=PFO_READY;
    o->v.accepted_arena=o->b.arenas[0];
    o->start_bytes=o->accepted_bytes=initial_bytes;
    memcpy(o->b.checkpoints[0],initial,initial_bytes);memcpy(o->b.checkpoints[1],initial,initial_bytes);
    return PFR_OK;
}
int pfo_get_view(const pfo_output *o,pfo_view *v){
    if(!o||o->ready!=READY)return PFR_FAILED;
    if(!v)return PFR_INVALID;
    *v=o->v;return PFR_OK;
}
int pfo_support(pfo_output *o,pfo_pressure pressure,pfo_project project){
    if(!o||o->ready!=READY||o->v.phase!=PFO_READY||o->v.accepted_next_chunk
       ||o->pressure||o->project||!pressure||!project)return PFR_INVALID;
    o->pressure=pressure;o->project=project;return PFR_OK;
}
int pfo_flush(pfo_output *o){
    pfr_view v;int rc=usable(o);
    if(rc!=PFR_OK)return rc;
    if(!o->v.pending_bytes)return PFR_OK;
    rc=pfr_commit(o->store,o->b.checkpoints[0],o->start_bytes,o->b.payload,o->v.pending_bytes);
    if(rc!=PFR_OK)return poison(o,rc);
    rc=pfr_get_view(o->store,&v);
    if(rc!=PFR_OK||v.next_chunk!=o->v.accepted_next_chunk||v.bound_payload_bytes!=o->v.accepted_payload_bytes)
        return poison(o,PFR_CORRUPT);
    o->v.committed_next_chunk=v.next_chunk;o->v.committed_payload_bytes=v.bound_payload_bytes;
    o->v.pending_bytes=0;
    if(!o->v.terminal&&o->project){size_t written=0;void *swap;
        rc=o->project(o->b.encoder_ctx,o->b.arenas[o->arena],o->b.arenas[o->arena^1u],
            v.first_chunk,o->b.checkpoints[2],o->b.checkpoint_capacity,&written);
        if(rc!=PFR_OK||!written||written>o->b.checkpoint_capacity)return poison(o,PFR_INVALID);
        swap=o->b.checkpoints[1];o->b.checkpoints[1]=o->b.checkpoints[2];o->b.checkpoints[2]=swap;
        o->accepted_bytes=written;o->arena^=1u;o->v.accepted_arena=o->b.arenas[o->arena];
    }
    if(!o->v.terminal){memcpy(o->b.checkpoints[0],o->b.checkpoints[1],o->accepted_bytes);o->start_bytes=o->accepted_bytes;}
    return PFR_OK;
}
static int candidate(pfo_output *o,const void *raw,size_t n,pfo_prepared *prepared){
    int rc;const uint8_t *p=raw;
    memset(prepared,0,sizeof *prepared);
    rc=o->b.prepare(o->b.encoder_ctx,o->b.arenas[o->arena],o->b.arenas[o->arena^1u],
        raw,n,o->b.checkpoints[2],o->b.checkpoint_capacity,prepared);
    if(rc==PFO_RESOURCE){
        if(prepared->reason<PFR_CHECKPOINT_BYTES||prepared->reason>PFR_COUNTER_RANGE)return poison(o,PFR_INVALID);
        return stopping(o,prepared->reason);
    }
    if(rc!=PFR_OK||prepared->reason||prepared->terminal!=(u32(p)==TRAILER))return poison(o,PFR_INVALID);
    if(prepared->checkpoint_bytes>o->b.checkpoint_capacity)return stopping(o,PFR_CHECKPOINT_BYTES);
    if((prepared->terminal&&prepared->checkpoint_bytes)||(!prepared->terminal&&!prepared->checkpoint_bytes))return poison(o,PFR_INVALID);
    return PFR_OK;
}
int pfo_accept(pfo_output *o,const void *raw,size_t n){
    const uint8_t *p=raw;pfo_prepared prepared;void *swap;uint64_t next,bytes;int pressure=0,rc=usable(o);
    if(rc!=PFR_OK)return rc;
    if(o->v.phase==PFO_STOPPING)return PFO_RESOURCE;
    if(o->v.terminal)return PFR_END;
    if(n<20||n>o->b.payload_capacity||!separate(o,raw,n))return PFR_INVALID;
    if(u32(p+4)>MAX_CHUNK||(uint64_t)u32(p+4)+20!=n||u64(p+8)!=o->v.accepted_next_chunk
       ||crc(p,n-4)!=u32(p+n-4))return PFR_INVALID;
    if(o->v.accepted_next_chunk==UINT64_MAX||n>UINT64_MAX-o->v.accepted_payload_bytes)
        return stopping(o,PFR_COUNTER_RANGE);
    next=o->v.accepted_next_chunk+1;bytes=o->v.accepted_payload_bytes+n;
    rc=candidate(o,raw,n,&prepared);if(rc!=PFR_OK)return rc;
    if(o->pressure&&!prepared.terminal){
        pressure=o->pressure(o->b.encoder_ctx,o->b.arenas[o->arena^1u]);
        if(pressure!=0&&pressure!=1)return poison(o,PFR_INVALID);
    }
    /* Do not acknowledge or swap state until any required old-segment commit
     * succeeds. The prepared checkpoint occupies its own third buffer. */
    if(n>o->b.payload_capacity-o->v.pending_bytes||(pressure&&o->v.pending_bytes)){
        rc=pfo_flush(o);if(rc!=PFR_OK)return rc;
        /* Projection uses the other arena/third checkpoint, replacing the
         * preflight candidate. Revalidate it against the projected predecessor
         * before acknowledging it; it has not entered any physical segment. */
        if(o->project){rc=candidate(o,raw,n,&prepared);if(rc!=PFR_OK)return rc;}
    }
    memcpy((uint8_t *)o->b.payload+o->v.pending_bytes,raw,n);
    if(!prepared.terminal){swap=o->b.checkpoints[1];o->b.checkpoints[1]=o->b.checkpoints[2];o->b.checkpoints[2]=swap;o->accepted_bytes=prepared.checkpoint_bytes;}
    o->arena^=1u;o->v.accepted_arena=o->b.arenas[o->arena];
    o->v.accepted_next_chunk=next;o->v.accepted_payload_bytes=bytes;o->v.pending_bytes+=n;o->v.terminal=prepared.terminal;
    return PFR_OK;
}
int pfo_finish(pfo_output *o){
    pfr_view v;pfr_transition t;int rc=usable(o);
    if(rc!=PFR_OK)return rc;
    if(o->v.phase!=PFO_STOPPING&&!o->v.terminal)return PFR_INVALID;
    rc=pfo_flush(o);if(rc!=PFR_OK)return rc;
    rc=pfr_get_view(o->store,&v);if(rc!=PFR_OK)return poison(o,rc);
    t.expected_revision=v.generation;t.next_chunk=o->v.accepted_next_chunk;t.payload_bytes=o->v.accepted_payload_bytes;
    t.state=o->v.phase==PFO_STOPPING?PFR_STOPPED_RESOURCE:PFR_SEALED;t.reason=o->v.reason;
    rc=pfr_publish_state(o->store,&t);if(rc!=PFR_OK)return poison(o,rc);
    o->v.phase=PFO_FINISHED;return PFR_OK;
}
