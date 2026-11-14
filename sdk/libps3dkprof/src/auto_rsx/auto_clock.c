#include "auto_clock.h"
#include <string.h>
#define ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define REL(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
_Static_assert(__atomic_always_lock_free(8,0),"AUTO clock needs inline counters");

int auto_clock_init(auto_clock *q,uint32_t instance,uint32_t epoch,uint32_t first,uint64_t frequency)
{
    if(!q || !instance || !epoch || first>2048-AUTO_CLOCK_SLOTS || !frequency || frequency>UINT64_MAX/2) return 0;
    memset(q,0,sizeof *q);q->instance=instance;q->epoch=epoch;q->first=first;q->frequency=frequency;
    return 1;
}
static void count(auto_clock *q,uint64_t *counter)
{
    uint64_t n=ACQ(counter);
    if(n==UINT64_MAX) REL(&q->saturated,1u);
    else REL(counter,n+1u);
}
/* Round-robin from the cursor, so reuse cycles through every free slot. */
static int free_slot(const auto_clock *q)
{
    for(uint32_t i=0;i<AUTO_CLOCK_SLOTS;i++) {
        uint32_t s=(q->cursor+i)%AUTO_CLOCK_SLOTS;
        if(ACQ(&q->slots[s])==AUTO_CLOCK_FREE) return (int)s;
    }
    return -1;
}
int auto_clock_room(const auto_clock *q)
{
    return q && ACQ(&q->write_idx)-ACQ(&q->read_idx)<AUTO_CLOCK_RECORDS &&
           q->serial<UINT32_MAX-1u && free_slot(q)>=0;
}
uint32_t auto_clock_before(const auto_clock *q) {return ACQ(&q->prepared);}
int auto_clock_after(auto_clock *q,uint32_t serial)
{
    if(serial>ACQ(&q->prepared) || serial<ACQ(&q->published)) return 0;
    REL(&q->published,serial);return 1;
}
/* An outcome is final: the record becomes DONE (visible to peek) with its release decision, written before. */
static void finish(auto_clock *q,uint64_t n)
{
    uint32_t i=(uint32_t)(n%AUTO_CLOCK_RECORDS);
    auto_wire_clock *r=&q->records[i];
    /* Reusable only after an exact valid completion (no write to the slot can still be outstanding) or when no
     * command word was written at all. */
    q->release[i]=r->flags==1u || !q->emitted[i];
    REL(&q->state[i],AUTO_CLOCK_DONE);
}
int auto_clock_sample(auto_clock *q,const auto_clock_port *p,uint32_t owner,uint64_t frame)
{
    uint64_t w;
    uint32_t index,serial,s;
    int found,rc;
    auto_wire_clock *r;
    if(!q || !p || !owner || q->active || !p->tb || !p->live || !p->emit || !p->flush ||
       !p->label || !p->timer || !p->sync) return 0;
    count(q,&q->attempted);
    w=ACQ(&q->write_idx);
    found=free_slot(q);
    /* No modular serial wrap: the label carries 2k, so k stops below UINT32_MAX/2. */
    if(w-ACQ(&q->read_idx)>=AUTO_CLOCK_RECORDS || found<0 || q->serial>=UINT32_MAX/2u-1u) {
        count(q,&q->lost);return 2;
    }
    s=(uint32_t)found;serial=q->serial+1u;q->serial=serial;
    index=(uint32_t)(w%AUTO_CLOCK_RECORDS);
    q->active=1;r=&q->records[index];memset(r,0,sizeof *r);
    r->instance=q->instance;r->epoch=q->epoch;r->id=r->serial=serial;r->slot=q->first+s;
    r->owner=owner;r->frame=frame;r->flags=8;
    q->emitted[index]=0;q->has_lo[index]=0;
    /* HELD before any device access: the drainer alone can return it. */
    REL(&q->slots[s],AUTO_CLOCK_HELD);
    q->cursor=(s+1u)%AUTO_CLOCK_SLOTS;
    r->tb_before=p->tb(p->user);q->issue[index]=r->tb_before;
    if(ACQ(&q->invalid) || !p->live(p->user)) goto failed;
    rc=p->emit(p->user,r->slot,serial);
    q->emitted[index]=rc!=0;
    if(rc!=1) goto failed;
    /* The metadata identity above exists before the original PUT path can publish this serial. */
    REL(&q->prepared,serial);
    p->flush(p->user);
    if(ACQ(&q->published)!=serial) goto failed;
    /* Issued: the drainer completes it; the owner never waits for the RSX. */
    REL(&q->state[index],AUTO_CLOCK_PENDING);
    q->active=0;REL(&q->write_idx,w+1u);
    return 1;
failed:
    /* Nothing reached the RSX in full: an immediate failure fact (READ_REFUSED), never polled. */
    r->tb_after=r->tb_before;
    finish(q,w);
    q->active=0;REL(&q->write_idx,w+1u);
    return 1;
}
const auto_wire_clock *auto_clock_peek(auto_clock *q)
{
    uint64_t r=ACQ(&q->read_idx);
    if(r==ACQ(&q->write_idx) || ACQ(&q->state[r%AUTO_CLOCK_RECORDS])!=AUTO_CLOCK_DONE) return NULL;
    return &q->records[r%AUTO_CLOCK_RECORDS];
}
int auto_clock_ack(auto_clock *q,uint32_t id,int whole)
{
    const auto_wire_clock *r=auto_clock_peek(q);
    uint64_t n;
    uint32_t s;
    if(!whole || !r || r->id!=id) return 0;
    n=ACQ(&q->read_idx);s=r->slot-q->first;
    if(q->release[n%AUTO_CLOCK_RECORDS]) REL(&q->slots[s],AUTO_CLOCK_FREE);
    else {REL(&q->slots[s],AUTO_CLOCK_QUARANTINED);REL(&q->quarantined,ACQ(&q->quarantined)+1u);}
    REL(&q->state[n%AUTO_CLOCK_RECORDS],AUTO_CLOCK_EMPTY);
    REL(&q->read_idx,n+1u);
    return 1;
}

int auto_clock_poll(auto_clock *q,const auto_clock_port *p,int seal)
{
    uint64_t n,w,now,before;
    uint32_t label;
    int live;
    if(!q || !p || !p->tb || !p->live || !p->label || !p->timer || !p->sync) return 1;
    w=ACQ(&q->write_idx);
    n=q->poll_idx;
    while(n<w && ACQ(&q->state[n%AUTO_CLOCK_RECORDS])==AUTO_CLOCK_DONE) n++;
    q->poll_idx=n;
    if(n==w) return 1;
    live=p->live(p->user) && !ACQ(&q->invalid);
    /* TB before the read bounds anything the read did not yet see; TB after bounds anything it saw. */
    before=p->tb(p->user);
    label=live ? p->label(p->user):0;
    now=p->tb(p->user);
    /* Cumulative label: below the last value this drainer saw is a regression, above the newest prepared anchor's
     * 2k a write nobody issued; either makes the instance invalid. */
    /* the bound is the newest serial the owner prepared before any PUT could reach the RSX, read after the label */
    if(live && (label<q->last_label || label>2u*ACQ(&q->prepared))) {
        REL(&q->invalid,1u);live=0;
    }
    if(live) q->last_label=label;
    for(;n<w;n++) {
        uint32_t i=(uint32_t)(n%AUTO_CLOCK_RECORDS),k;
        auto_wire_clock *r=&q->records[i];
        if(ACQ(&q->state[i])!=AUTO_CLOCK_PENDING) continue;
        k=r->serial;
        if(!live) {r->tb_after=now;r->elapsed=now>=r->tb_before ? now-r->tb_before:0;finish(q,n);continue;}
        if(r->polls<AUTO_CLOCK_POLLS) r->polls++;
        if(label>=2u*k) {
            uint64_t raw,repeated,after;
            uint32_t second;
            p->sync(p->user);raw=p->timer(p->user,r->slot);
            p->sync(p->user);repeated=p->timer(p->user,r->slot);
            p->sync(p->user);second=p->label(p->user);after=p->tb(p->user);
            r->label_second=label;r->tb_after=now;
            if(second<label) {REL(&q->invalid,1u);live=0;r->elapsed=0;finish(q,n);continue;}
            if(after<now || now<r->tb_before) {r->flags=4;r->elapsed=0;finish(q,n);continue;}
            r->elapsed=now-r->tb_before;
            if(raw!=UINT64_MAX && raw==repeated) {r->raw=raw;r->repeated=repeated;r->flags=1;}
            finish(q,n);continue;
        }
        if(label<2u*k-1u) {
            /* the RSX is not yet at this anchor's timestamp: the latest such poll is the lower bound */
            if(before<r->tb_before || now<before) {r->flags=4;r->tb_after=now;r->elapsed=0;finish(q,n);continue;}
            r->tb_before=before;r->label_first=label;q->has_lo[i]=1;
        }
        if(seal || now-q->issue[i]>=q->frequency*2u) {
            r->flags=2;r->tb_after=now;r->elapsed=now-r->tb_before;finish(q,n);
        }
    }
    while(q->poll_idx<w && ACQ(&q->state[q->poll_idx%AUTO_CLOCK_RECORDS])==AUTO_CLOCK_DONE) q->poll_idx++;
    return !ACQ(&q->invalid);
}
