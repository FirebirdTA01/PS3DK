#include "wait_internal.h"
#include <string.h>
#ifdef PS3DK_PROF_TEST_HOOKS
void (*ps3dk_prof__wait_test_hook)(unsigned);
#define W_HOOK(n) do {if(ps3dk_prof__wait_test_hook)ps3dk_prof__wait_test_hook(n);}while(0)
#else
#define W_HOOK(n) ((void)0)
#endif

/* Bounded contention is reported as saturation/lower-bound evidence. */
static int count(uint64_t *p,uint32_t *sat)
{
    uint64_t n=LD_RLX(p);
    for(unsigned i=0;i<4;++i){
        if(n==UINT64_MAX){ST_RLX(sat,1);return 0;}
        W_HOOK(40u); /* test-only competing refusal between load and CAS */
        if(__atomic_compare_exchange_n(p,&n,n+1,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))return 1;
    }
    ST_RLX(sat,1);
    return 0;
}
void p_wait_enroll(prof *p,uint32_t inc,uint32_t generation)
{
    wait_actor *a=p_wait_actor(p->waits,inc);if(!a)return;
    a->generation=generation;ST_REL(&a->valid,inc);
}
void p_wait_ack(prof *p)
{
    wait_actor *a;
    if(g_tls.wait_busy || !p->waits)return;
    a=p_wait_actor(p->waits,g_tls.incarnation);
    if(a && LD_ACQ(&a->valid)==g_tls.incarnation && a->generation==g_tls.generation)
        ST_REL(&a->ack,1); /* no producer changes this actor again */
}
static void publish(prof *p,wait_actor *a,uint8_t kind,int32_t raw,int32_t api)
{
    wait_capture *w=p->waits;uint64_t seq=LD_RLX(&a->seq)+1,head=LD_RLX(&a->head);
    ST_RLX(&a->seq,seq);
    W_HOOK(kind==1?10u:20u);
    a->pending[6]=kind;p_le64(a->pending+8,seq);p_le64(a->pending+24,p_tb());
    p_le32(a->pending+80,(uint32_t)raw);p_le32(a->pending+84,(uint32_t)api);
    if(head-LD_ACQ(&a->tail)>=w->cfg.ring_records){count(&a->dropped,&a->saturated);return;}
    memcpy(p_wait_record(w,a,head),a->pending,W_REC);
    W_HOOK(kind==1?11u:21u);
    ST_REL(&a->head,head+1);
    W_HOOK(kind==1?12u:22u);
}
ps3dk_prof_wait_cookie ps3dk_prof__wait_begin(uint32_t op,uint64_t target,uint64_t secondary,uint64_t timeout,uint64_t site)
{
    ps3dk_prof_wait_cookie c={0};prof *p;wait_capture *w;wait_actor *a;
    if(!p_ready())return c;
    p=g_prof;w=p->waits;
    if(!w || LD_ACQ(&p->stop_latch))return c;
    /* Internal acquisitions never become application rows, even before thread
     * registration. Drain and sink callbacks share the drainer's scope. */
    if(g_tls.wait_busy==2 || g_tls.in_drain || g_tls.in_gpu){
        if(count(&w->refused,&w->saturated))count(&w->internal_excluded,&w->saturated);
        return c;
    }
    a=p_wait_actor(w,g_tls.incarnation);
    if(!a || !g_tls.slot1 || g_tls.retired || g_tls.acked || LD_ACQ(&a->valid)!=g_tls.incarnation){
        /* Capacity-refused known incarnations are derived from THREADS+CONFIG. */
        if(!g_tls.slot1 && count(&w->refused,&w->saturated))count(&w->unregistered,&w->saturated);
        return c;
    }
    if(LD_ACQ(&a->ack) || a->generation!=g_tls.generation)return c;
    if(g_tls.wait_busy || a->active){
        if(count(&a->refused,&a->saturated))count(&a->nested,&a->saturated);
        return c;
    }
    if(op<1 || op>3 || (op==1 && timeout) || (op!=3 && secondary) || LD_RLX(&a->seq)>UINT64_MAX-3){
        count(&a->refused,&a->saturated);return c;
    }
    g_tls.wait_busy=1;a->active=1;
    memset(a->pending,0,W_REC);p_le32(a->pending,g_tls.incarnation);p_le16(a->pending+4,(uint16_t)op);
    c.session=p->tb_at_init;c.actor=g_tls.incarnation;c.generation=g_tls.generation;
    c.call_id=LD_RLX(&a->seq)+1;c.admitted=1;
    p_le64(a->pending+16,c.call_id);p_le64(a->pending+32,target);p_le64(a->pending+40,secondary);
    p_le64(a->pending+48,timeout);p_le64(a->pending+56,site);p_le32(a->pending+68,2u+(!site));
    publish(p,a,1,0,0);g_tls.wait_busy=0;return c;
}
void ps3dk_prof__wait_end(ps3dk_prof_wait_cookie c,int32_t raw,int32_t api)
{
    prof *p;wait_actor *a;
    if(!c.admitted || !p_ready())return;
    p=g_prof;
    if(c.session!=p->tb_at_init || c.actor!=g_tls.incarnation || c.generation!=g_tls.generation ||
       g_tls.wait_busy || g_tls.retired || g_tls.acked)return;
    a=p_wait_actor(p->waits,c.actor);
    if(!a || LD_ACQ(&a->ack) || !a->active || LD_RLX(&a->seq)!=c.call_id)return;
    if(LD_ACQ(&p->stop_latch)){p_wait_ack(p);return;}
    g_tls.wait_busy=1;publish(p,a,2,raw,api);a->active=0;g_tls.wait_busy=0;
}
void __ps3dk_prof_wait_thread_start(void)
{
    if(p_ready() && !g_tls.in_drain && !g_tls.wait_busy)(void)ps3dk_prof_thread_begin("thread");
}
void __ps3dk_prof_wait_thread_exit(void){ps3dk_prof_thread_end();}
