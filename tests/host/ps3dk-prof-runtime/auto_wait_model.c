#include "auto_wait_model.h"
static void count(awm_model *m,uint64_t *value) {
    if(*value==UINT64_MAX)m->loss=1;else ++*value;
}
static void clock_read(awm_model *m,uint64_t tb) {
    if(m->have_tb && tb<m->last_tb)m->loss=1;
    m->last_tb=tb;m->have_tb=1;
}
uint64_t awm_begin(awm_model *m,uint32_t actor,uint32_t context,uint32_t reason,uint32_t domain,uint64_t parent,uint32_t owner,uint64_t frame,uint64_t tb,uint64_t target,uint64_t value)
{
    const awm_record *top=m->depth?&m->records[m->stack[m->depth-1]]:0;
    if(m->stopped || !actor || !context || !reason || domain<AWM_CPU_POLL || domain>AWM_INTERNAL || (!owner && frame)
       || m->used==AWM_RECORDS || m->depth==AWM_DEPTH || m->next_id==UINT64_MAX
       || parent!=(top?top->id:0) || (top && (top->actor!=actor || top->context!=context))) {
        count(m,&m->refused);m->loss=1;return 0;
    }
    clock_read(m,tb);
    unsigned index=m->used++;awm_record *r=&m->records[index];
    *r=(awm_record){0};r->id=++m->next_id;r->parent=parent;r->frame=frame;r->begin=tb;
    r->target=target;r->value=value;r->actor=actor;r->context=context;r->reason=reason;r->domain=domain;r->owner=owner;
    m->stack[m->depth++]=index;
    if(domain==AWM_GPU_COMMAND)count(m,&m->commands);
    if(domain==AWM_INTERNAL)count(m,&m->internal);
    return r->id;
}
int awm_end(awm_model *m,uint32_t actor,uint64_t id,uint64_t tb,uint32_t outcome)
{
    awm_record *r=m->depth?&m->records[m->stack[m->depth-1]]:0;
    if(m->stopped || !r || !id || r->id!=id || r->actor!=actor || outcome<AWM_SUCCESS || outcome>AWM_DEADLINE) {
        count(m,&m->orphan);m->loss=1;return 0;
    }
    int regressed=tb<r->begin || (m->have_tb && tb<m->last_tb);clock_read(m,tb);
    --m->depth;r->end=tb;r->outcome=regressed?AWM_CLOCK:outcome;
    r->valid=!regressed && r->domain!=AWM_GPU_COMMAND;
    if(r->valid)r->duration=tb-r->begin;
    return 1;
}
void awm_stop(awm_model *m) {
    m->stopped=1;
    while(m->depth){awm_record *r=&m->records[m->stack[--m->depth]];r->outcome=AWM_STOPPED;r->valid=0;}
}
int awm_cpu_total(const awm_model *m,uint64_t *ticks) {
    if(m->loss || m->depth)return 0;
    uint64_t total=0,points[AWM_RECORDS*2];unsigned n=0;
    for(unsigned i=0;i<m->used;i++) {
        const awm_record *r=&m->records[i];
        if(r->outcome==AWM_STOPPED || r->outcome==AWM_OPEN || r->outcome==AWM_CLOCK)return 0;
        if(r->domain==AWM_GPU_COMMAND)continue;
        if(!r->valid)return 0;
        points[n++]=r->begin;points[n++]=r->end;
    }
    /* Bounded interval union with internal intervals removed, in either nesting direction. */
    for(unsigned i=1;i<n;i++){uint64_t p=points[i];unsigned j=i;while(j && points[j-1]>p){points[j]=points[j-1];--j;}points[j]=p;}
    for(unsigned i=1;i<n;i++) {
        unsigned cpu=0,internal=0;uint64_t lo=points[i-1],hi=points[i];
        for(unsigned j=0;j<m->used;j++) {
            const awm_record *r=&m->records[j];
            if(r->begin>lo || r->end<hi)continue;
            if(r->domain==AWM_CPU_POLL || r->domain==AWM_CPU_CALL_OPAQUE)cpu++;
            if(r->domain==AWM_INTERNAL)internal=1;
        }
        if(cpu && !internal){uint64_t delta=hi-lo;if(delta>UINT64_MAX-total)return 0;total+=delta;}
    }
    *ticks=total;return 1;
}
