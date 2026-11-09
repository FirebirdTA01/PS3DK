#include "wait_internal.h"
#include <string.h>
#ifndef PS3DK_PROF_WAIT_STACK_BYTES
#error "wait variants require their compiler-audited stack bound"
#endif
#ifndef PS3DK_PROF_WAIT_STATIC_BYTES
#define PS3DK_PROF_WAIT_STATIC_BYTES sizeof(p_wait_options)
#endif
#ifndef PS3DK_PROF_WAIT_TLS_BYTES
#define PS3DK_PROF_WAIT_TLS_BYTES sizeof(g_tls.wait_busy)
#endif
static uint64_t get64(const uint8_t *p)
{
    uint64_t n=0;for(unsigned i=0;i<8;++i)n|=(uint64_t)p[i]<<(8*i);return n;
}
void p_wait_declared(prof *p,const tdecl *decl,uint32_t n)
{
    for(uint32_t i=0;i<n;++i){wait_actor *a=p_wait_actor(p->waits,decl[i].incarnation);
        if(a && LD_ACQ(&a->valid)==decl[i].incarnation && a->generation==decl[i].generation)a->declared=1;
    }
}
static int config(prof *p)
{
    wait_capture *w=p->waits;uint8_t b[56]={0};if(w->configured)return 0;
    p_le16(b,1);p_le32(b+4,w->cfg.max_actors);p_le32(b+8,w->cfg.ring_records);
    p_le32(b+12,w->cfg.max_bytes);p_le32(b+16,w->allocated);p_le32(b+20,W_REC);
    p_le64(b+24,p->tb_freq);p_le64(b+32,7);
    p_le32(b+40,PS3DK_PROF_WAIT_STATIC_BYTES);p_le32(b+44,PS3DK_PROF_WAIT_TLS_BYTES);
    p_le32(b+48,PS3DK_PROF_WAIT_STACK_BYTES);
    if(p_wait_emit_chunk(p,22,b,sizeof b))return -1;
    w->configured=1;return 0;
}
int p_wait_emit_ready(prof *p)
{
    wait_capture *w=p->waits;if(!w || w->status || p->snk.failed)return -1;
    if(config(p))return -1;
    for(uint32_t inc=1;inc<=w->cfg.max_actors;++inc){
        wait_actor *a=p_wait_actor(w,inc);if(!LD_ACQ(&a->valid) || !a->declared)continue;
        uint64_t limit=w->frozen?a->frontier:LD_ACQ(&a->head),tail=LD_RLX(&a->tail);
        while(tail<limit){
            uint8_t *b=p->dr.buf+16;uint64_t n=limit-tail;
            if(n>(P_CHUNK_CAP-8)/W_REC)n=(P_CHUNK_CAP-8)/W_REC;
            p_le16(b,1);p_le16(b+2,0);p_le32(b+4,(uint32_t)n);
            for(uint64_t i=0;i<n;++i)memcpy(b+8+(size_t)i*W_REC,p_wait_record(w,a,tail+i),W_REC);
            uint64_t last=get64(b+8+(size_t)(n-1)*W_REC+8);
            if(p_wait_emit_chunk(p,23,b,8+(size_t)n*W_REC))return -1;
            a->written+=n;a->last=last;tail+=n;ST_REL(&a->tail,tail);
        }
    }
    return 0;
}
void p_wait_freeze(prof *p)
{
    wait_capture *w=p->waits;if(!w || w->frozen)return;
    for(uint32_t inc=1;inc<=w->cfg.max_actors;++inc){
        wait_actor *a=p_wait_actor(w,inc);if(!LD_ACQ(&a->valid))continue;
        uint32_t ack=LD_ACQ(&a->ack);a->frontier=LD_ACQ(&a->head);
        a->final_flags=ack?0u:5u;
        a->final_drop=LD_RLX(&a->dropped);
        /* A subset's release follows its total. Read subset first; clamp keeps
         * pre-existing saturated states and unacknowledged samples conservative. */
        a->final_nested=LD_ACQ(&a->nested);a->final_refused=LD_ACQ(&a->refused);
        if(a->final_nested>a->final_refused)a->final_nested=a->final_refused;
        a->final_attempted=ack?LD_RLX(&a->seq):0;
        if(LD_RLX(&a->saturated))a->final_flags|=3;
    }
    w->frozen=1;
}
int p_wait_emit_status(prof *p)
{
    wait_capture *w=p->waits;uint8_t *b=p->dr.buf+16;uint32_t n=0;
    if(!w || w->status || !w->frozen || p_wait_emit_ready(p))return -1;
    memset(b,0,8+64);p_le16(b,1);
    /* Aggregate counters remain lower bounds: unregistered producers have no ACK. */
    uint64_t internal=LD_ACQ(&w->internal_excluded),unregistered=LD_ACQ(&w->unregistered),refused=LD_ACQ(&w->refused);
    if(internal>refused)internal=refused;
    if(unregistered>refused-internal)unregistered=refused-internal;
    p_le64(b+8+40,internal);
    p_le32(b+12,LD_RLX(&w->saturated)?3:1);p_le64(b+8+32,refused);p_le64(b+8+48,unregistered);
    for(uint32_t inc=1;inc<=w->cfg.max_actors;++inc){
        wait_actor *a=p_wait_actor(w,inc);if(!LD_ACQ(&a->valid) || !a->declared)continue;
        uint8_t *r=b+72+64*n++;memset(r,0,64);p_le32(r,inc);p_le32(r+4,a->final_flags);
        p_le64(r+8,a->last);p_le64(r+16,a->written);p_le64(r+24,a->final_drop);
        p_le64(r+32,a->final_refused);p_le64(r+40,a->final_nested);p_le64(r+56,a->final_attempted);
    }
    p_le32(b+4,n);if(p_wait_emit_chunk(p,24,b,72+(size_t)n*64))return -1;
    w->status=1;return 0;
}
