#include "auto_rsx_model.h"
#include <string.h>
#include "ps3tc_fifo_wrap_protocol.h"

static void add(arm_model *m,uint64_t *n,uint64_t delta)
{
    if(delta>UINT64_MAX-*n){*n=UINT64_MAX;m->coverage_lost=1;}
    else *n+=delta;
}
static void barrier(void *user) { (void)user; }
static uint64_t now(void *user) { return ++((arm_model *)user)->clock; }
static void pause_wait(void *user,uint32_t want,uint32_t spins,unsigned phase,uint64_t elapsed)
{
    arm_model *m=user;(void)spins;(void)elapsed;
    if(phase>=1 && phase<=2)m->phases[phase-1]++;
    m->get=want; /* fake device progress, not a hardware premise */
}
uint64_t arm_words(const arm_operation *op)
{
    uint64_t count=op->count;
    if(op->kind==ARM_CLEAR) return 6;
    if(!count || count>0xffffffu) return 0;
    if(op->kind==ARM_ARRAY) {
        uint64_t batches=(count-1)/256;
        return 10+(batches/2047)*2048+(batches%2047 ? 1+batches%2047 : 0);
    }
    if(op->kind==ARM_INDEX && op->index_type<=1) {
        uint64_t mis=((128-(op->offset&127))&127)>>(op->index_type ? 1 : 2);
        uint64_t odd=mis && mis<count;
        if(odd) count-=mis;
        uint64_t loops=(count-1)/0x7ff00;
        uint64_t rest=count-loops*0x7ff00;
        return 10+odd*2+loops*2048+(rest+255)/256;
    }
    return 0;
}
int arm_words_to_bytes(uint64_t words, uint64_t *bytes)
{
    if(words>UINT64_MAX/4) return 0;
    *bytes=words*4;
    return 1;
}

void arm_init(arm_model *m,uint32_t *ring,uint32_t capacity,uint32_t pairs,uint32_t metadata)
{
    memset(m,0,sizeof *m);m->ring=ring;m->capacity=capacity;m->pair_limit=pairs;
    m->metadata_limit=metadata;m->context=1;m->generation=1;m->next_serial=1;m->label=240;
    if(!ring || !capacity || capacity>=(1u<<27) || pairs>ARM_MAX_PAIRS){m->context=0;m->pair_limit=0;}
    m->frames_used=1;m->app_hash=UINT64_C(14695981039346656037);
}
int arm_frame_open(arm_model *m,uint32_t owner,uint64_t frame)
{
    if(!owner || m->frames_used==ARM_MAX_FRAMES)return 0;
    for(uint32_t i=0;i<m->frames_used;i++)if(m->frames[i].owner==owner && m->frames[i].id==frame)return 0;
    arm_frame *f=&m->frames[m->frames_used++];f->owner=owner;f->id=frame;return 1;
}
int arm_frame_close(arm_model *m,uint32_t owner,uint64_t frame)
{
    for(uint32_t i=1;i<m->frames_used;i++)if(m->frames[i].owner==owner && m->frames[i].id==frame && !m->frames[i].closed){m->frames[i].closed=1;return 1;}
    return 0;
}
int arm_emit_packet(arm_model *m,const uint32_t *words,uint32_t n)
{
    if(!m->ring || m->current>m->capacity || n>m->capacity)return 1;
    if(n>m->capacity-m->current){
        ps3tc_fifo_port p={&m->put,&m->get,barrier,now,pause_wait,m};
        ps3tc_fifo_wrap_protocol(m->ring+m->current,m->current*4u,0,&p);
        m->current=0;add(m,&m->wraps,1);
    }
    memcpy(m->ring+m->current,words,n*4u);m->current+=n;
    for(uint32_t i=0;i<n;i++){m->app_hash^=words[i];m->app_hash*=UINT64_C(1099511628211);}
    add(m,&m->app_words,n);return 0;
}
static int marker(arm_model *m,uint32_t slot,uint32_t serial)
{
    if(m->current>m->capacity || m->capacity-m->current<6)return 0;
    uint32_t w[6]={0x00041800u,0x01000000u|(m->report_base+slot)*16u,0x00041d6cu,m->label*16u,
        0x00041d70u,(serial&0xff00ff00u)|((serial>>16)&0xffu)|((serial&0xffu)<<16)};
    memcpy(m->ring+m->current,w,sizeof w);m->current+=6;add(m,&m->marker_words,6);return 1;
}
static arm_frame *frame(arm_model *m,const arm_operation *op)
{
    for(uint32_t i=1;i<m->frames_used;i++)if(m->frames[i].owner==op->frame_owner && m->frames[i].id==op->frame && !m->frames[i].closed)return &m->frames[i];
    return &m->frames[0]; /* explicitly unassociated, never a fabricated frame */
}
arm_result arm_submit(arm_model *m,const arm_operation *op,arm_emitter emit,void *user)
{
    arm_result r={ARM_OBSERVED,0,0};uint64_t n=arm_words(op);uint32_t free_slot=ARM_MAX_PAIRS,used=0;
    arm_frame *f=frame(m,op);unsigned kind=op->kind>=ARM_ARRAY && op->kind<=ARM_CLEAR ? op->kind-1 : 3;
    if(kind<3)add(m,&f->visited[kind],1);else add(m,&f->unsupported,1);
    for(uint32_t i=0;i<m->pair_limit && i<ARM_MAX_PAIRS;i++) {
        if(m->tickets[i].state==ARM_FREE || m->tickets[i].state==ARM_RETIRED){if(free_slot==ARM_MAX_PAIRS)free_slot=i;}
        else used++;
    }
    if(!n)r.reason=ARM_UNSUPPORTED;
    else if(!m->context || op->context!=m->context || op->generation!=m->generation || m->stopped || m->current>m->capacity)r.reason=ARM_CONTEXT;
    else if(m->busy)r.reason=ARM_REENTRY;
    else if(n>UINT64_MAX-12)r.reason=ARM_SIZE;
    else if(m->report_conflict || free_slot==ARM_MAX_PAIRS || !m->next_serial || m->next_serial>UINT32_MAX-2 || m->next_ticket==UINT64_MAX)r.reason=ARM_REPORTS;
    else if(used>=m->metadata_limit)r.reason=ARM_METADATA;
    else if(n+12>m->capacity-m->current)r.reason=ARM_ROOM;
    arm_ticket *t=0;
    if(!r.reason) {
        t=&m->tickets[free_slot];memset(t,0,sizeof *t);t->state=ARM_CLAIMED;t->generation=m->generation;
        t->frame_owner=f->owner;t->frame=f->id;t->kind=op->kind;
        t->id=++m->next_ticket;t->serial[0]=m->next_serial;t->serial[1]=m->next_serial+1;m->next_serial+=2;
        r.ticket=t->id;m->busy=1;
        if(!marker(m,free_slot*2,t->serial[0]))r.reason=ARM_EMISSION;
    }
    uint64_t before=m->app_words;
    add(m,&m->calls,1);r.application_result=emit(m,op,user);
    if(t) {
        if(r.reason || r.application_result || m->app_words-before!=n || !marker(m,free_slot*2+1,t->serial[1])){
            t->state=ARM_QUARANTINE;r.reason=ARM_EMISSION;
        }else t->state=ARM_EMITTED;
        m->busy=0;
    }
    if(kind<3){if(!r.reason)add(m,&f->observed[kind],1);else add(m,&f->excluded[kind][r.reason],1);}
    return r;
}
int arm_publish(arm_model *m,uint32_t context,uint32_t generation,uint64_t frontier)
{
    if(m->stopped || !context || context!=m->context || generation!=m->generation || frontier>m->next_ticket)return 0;
    for(uint32_t i=0;i<m->pair_limit;i++) {
        arm_ticket *t=&m->tickets[i];
        if(t->id<=frontier && t->state==ARM_EMITTED)t->state=ARM_PUBLISHED;
    }
    return 1;
}
static arm_ticket *ticket(arm_model *m,uint64_t id)
{
    if(!id)return 0;
    for(uint32_t i=0;i<m->pair_limit;i++)if(m->tickets[i].id==id)return &m->tickets[i];
    return 0;
}
int arm_gpu_complete(arm_model *m,uint64_t id,uint32_t generation,unsigned half,uint32_t serial,uint64_t raw)
{
    arm_ticket *t=ticket(m,id);
    if(!t || t->state!=ARM_PUBLISHED || generation!=t->generation || half>1 || serial!=t->serial[half] || raw==UINT64_MAX || (t->complete&(1u<<half)))return 0;
    t->raw[half]=raw;t->complete|=1u<<half;
    if(t->complete==3)t->state=t->raw[1]>=t->raw[0]?ARM_COMPLETE:ARM_QUARANTINE;
    return 1;
}
int arm_sink_result(arm_model *m,uint64_t id,int accepted_whole)
{
    arm_ticket *t=ticket(m,id);
    if(!t || t->state!=ARM_COMPLETE || accepted_whole!=1)return 0;
    t->metadata_accepted=1;t->state=ARM_RETIRED;return 1;
}
int arm_drain_offer(arm_model *m,uint64_t id)
{
    arm_ticket *t=ticket(m,id);return t && t->state==ARM_COMPLETE;
}
void arm_stop(arm_model *m)
{
    m->stopped=1;
    for(uint32_t i=0;i<m->pair_limit;i++) {
        arm_ticket *t=&m->tickets[i];
        if(t->state!=ARM_FREE && t->state!=ARM_RETIRED)t->state=ARM_QUARANTINE;
    }
}
int arm_claim_reports(arm_registry *r,uint32_t owner,uint32_t generation,uint32_t first,uint32_t count,uint32_t label)
{
    if(!owner || !generation || !count || first>=2048 || count>2048-first || !(label==64 || (label>=69 && label<255)))return 0;
    unsigned free_slot=ARM_REGISTRY_SIZE;
    for(unsigned i=0;i<ARM_REGISTRY_SIZE;i++) {
        const arm_claim *c=&r->claims[i];
        if(!c->owner){if(free_slot==ARM_REGISTRY_SIZE)free_slot=i;continue;}
        if(c->owner==owner || c->label==label || (first<c->first+c->count && c->first<first+count))return 0;
    }
    if(free_slot==ARM_REGISTRY_SIZE)return 0;
    r->claims[free_slot]=(arm_claim){owner,generation,first,count,label};return 1;
}
int arm_release_reports(arm_registry *r,uint32_t owner,uint32_t generation,const arm_model *m)
{
    if(!m->stopped || owner!=m->context || generation!=m->generation)return 0;
    for(unsigned i=0;i<m->pair_limit;i++)if(m->tickets[i].state!=ARM_FREE && m->tickets[i].state!=ARM_RETIRED)return 0;
    for(unsigned i=0;i<ARM_REGISTRY_SIZE;i++)if(r->claims[i].owner==owner && r->claims[i].generation==generation){memset(&r->claims[i],0,sizeof r->claims[i]);return 1;}
    return 0;
}
int arm_bind_reports(arm_model *m,arm_registry *r,uint32_t first,uint32_t count,uint32_t label)
{
    /* Before first emission only. Fixtures without a registry assume exclusive ownership. */
    if(m->next_ticket || m->stopped)return 0;
    if(count<m->pair_limit*2 || !arm_claim_reports(r,m->context,m->generation,first,count,label)) {
        m->report_conflict=1;return 0;
    }
    m->report_base=first;m->label=label;m->report_conflict=0;return 1;
}
