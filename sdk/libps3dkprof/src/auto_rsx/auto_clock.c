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
/* Inspect the ordered TB before accepting a match and again after reading it.
 * A late match is a timeout; clock regression is never a substituted duration. */
static int elapsed(auto_clock *q,auto_wire_clock *r,uint64_t now,uint64_t previous)
{
    r->tb_after=now;
    if(now<previous || now<r->tb_before) {r->flags=4;r->elapsed=0;return 0;}
    r->elapsed=now-r->tb_before;
    if(r->elapsed>=q->frequency*2u) {r->flags=2;return 0;}
    return 1;
}
int auto_clock_sample(auto_clock *q,const auto_clock_port *p,uint32_t owner,uint64_t frame)
{
    uint64_t w,prior;
    uint32_t index,serial,s,emitted=0,release;
    int found;
    auto_wire_clock *r;
    if(!q || !p || !owner || q->active || !p->tb || !p->live || !p->emit || !p->flush ||
       !p->label || !p->timer || !p->sync) return 0;
    count(q,&q->attempted);
    w=ACQ(&q->write_idx);
    found=free_slot(q);
    /* No modular serial wrap: UINT32_MAX is never issued, as for operations. */
    if(w-ACQ(&q->read_idx)>=AUTO_CLOCK_RECORDS || found<0 || q->serial>=UINT32_MAX-1u) {
        count(q,&q->lost);return 2;
    }
    s=(uint32_t)found;serial=q->serial+1u;q->serial=serial;
    index=(uint32_t)(w%AUTO_CLOCK_RECORDS);
    q->active=1;r=&q->records[index];memset(r,0,sizeof *r);
    r->instance=q->instance;r->epoch=q->epoch;r->id=r->serial=serial;r->slot=q->first+s;
    r->owner=owner;r->frame=frame;r->flags=8;
    /* HELD before any device access: the drainer alone can return it. */
    REL(&q->slots[s],AUTO_CLOCK_HELD);
    q->cursor=(s+1u)%AUTO_CLOCK_SLOTS;
    r->tb_before=prior=p->tb(p->user);
    if(q->invalid || !p->live(p->user) || !p->emit(p->user,r->slot,r->serial)) {
        (void)elapsed(q,r,p->tb(p->user),prior);goto publish;
    }
    emitted=1;
    /* The metadata identity above exists before the original PUT path can
     * publish this serial. The final readback fact is published separately. */
    REL(&q->prepared,r->serial);
    p->flush(p->user);
    if(ACQ(&q->published)!=r->serial) {
        (void)elapsed(q,r,p->tb(p->user),prior);goto publish;
    }
    for(r->polls=1;r->polls<=AUTO_CLOCK_POLLS;r->polls++) {
        uint64_t now,raw,repeated;
        r->label_first=p->label(p->user);now=p->tb(p->user);
        if(!elapsed(q,r,now,prior)) goto publish;
        prior=now;
        if(!p->live(p->user)) goto publish;
        /* Cumulative label: below the last accepted value is a regression, above
         * this serial a future write; both invalidate. Between them is an
         * earlier (for example timed-out) anchor completing late: keep polling. */
        if(r->label_first<q->last_label || r->label_first>r->serial) {q->invalid=1;goto publish;}
        q->last_label=r->label_first;
        if(r->label_first!=r->serial) continue;
        p->sync(p->user);raw=p->timer(p->user,r->slot);
        p->sync(p->user);repeated=p->timer(p->user,r->slot);
        p->sync(p->user);r->label_second=p->label(p->user);now=p->tb(p->user);
        if(!elapsed(q,r,now,prior)) goto publish;
        if(!p->live(p->user)) goto publish;
        if(r->label_second!=r->serial) {q->invalid=1;goto publish;}
        if(raw==UINT64_MAX || raw!=repeated) goto publish;
        r->raw=raw;r->repeated=repeated;r->flags=1;goto publish;
    }
    r->polls=AUTO_CLOCK_POLLS;r->flags=2;
publish:
    /* Reusable only after an exact valid completion (no write to the slot can
     * still be outstanding) or when no command word was written at all. */
    release=r->flags==1u || !emitted;
    q->release[index]=release;
    q->active=0;
    REL(&q->write_idx,w+1u);
    return 1;
}
const auto_wire_clock *auto_clock_peek(auto_clock *q)
{
    uint64_t r=ACQ(&q->read_idx);
    return r==ACQ(&q->write_idx) ? NULL:&q->records[r%AUTO_CLOCK_RECORDS];
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
    REL(&q->read_idx,n+1u);
    return 1;
}
