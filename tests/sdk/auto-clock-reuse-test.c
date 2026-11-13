/* Clock-anchor slot reuse lifetime controls. The fake device executes published
 * commands in order and holds them on request; it supplies facts, it is not
 * evidence of RSX ordering, report-before-label behaviour or timer rate. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auto_clock.h"

enum { FIRST=1024, FREQ=100 };
static auto_clock q;
typedef struct command { uint32_t slot, serial, published; } command;
static struct {
    uint64_t tick, clock, reports[2048];
    uint32_t label, hold, burst, unpublished, no_room, future, regress, stop_at_label, labels_read;
    uint32_t emits, head, tail, live;
    command fifo[64];
} dev;

static uint64_t tb(void *u) {(void)u;return dev.tick++;}
static int live(void *u) {(void)u;return dev.live;}
static int emit(void *u,uint32_t slot,uint32_t serial)
{
    (void)u;
    assert(slot>=FIRST && slot<FIRST+AUTO_CLOCK_SLOTS);
    if(dev.no_room) return 0;
    /* A slot is never handed out while an earlier command for it is queued. */
    for(uint32_t i=dev.head;i<dev.tail;i++) assert(dev.fifo[i%64].slot!=slot);
    assert(dev.tail-dev.head<64);
    dev.reports[slot]=UINT64_MAX; /* poison precedes the command */
    dev.fifo[dev.tail++%64]=(command){slot,serial,0};
    dev.emits++;
    return 1;
}
static void flush(void *u)
{
    (void)u;
    uint32_t serial=auto_clock_before(&q);
    if(dev.unpublished) return;
    for(uint32_t i=dev.head;i<dev.tail;i++) dev.fifo[i%64].published=1;
    assert(auto_clock_after(&q,serial));
}
/* In-order execution of published commands; a held device runs nothing. */
static void run_one(void)
{
    command *c=&dev.fifo[dev.head%64];
    if(dev.head==dev.tail || !c->published) return;
    dev.reports[c->slot]=++dev.clock*1000u;
    dev.label=c->serial;dev.head++;
}
static void run_all(void) {for(int i=0;i<64;i++) run_one();}
static uint32_t label(void *u)
{
    (void)u;
    if(++dev.labels_read==dev.stop_at_label) dev.live=0;
    if(dev.burst) run_all();
    else if(!dev.hold) run_one();
    if(dev.future) return dev.label+1u;
    if(dev.regress) return dev.label-2u; /* below the last accepted label */
    return dev.label;
}
static uint64_t timer(void *u,uint32_t slot) {(void)u;return dev.reports[slot];}
static void sync_device(void *u) {(void)u;}
static const auto_clock_port port={NULL,tb,live,emit,flush,label,timer,sync_device};

static void setup(void)
{
    memset(&dev,0,sizeof dev);dev.tick=1;dev.live=1;
    assert(auto_clock_init(&q,1,1,FIRST,FREQ));
}
static const auto_wire_clock *sample(uint64_t frame)
{
    uint64_t w=q.write_idx;
    assert(auto_clock_sample(&q,&port,7,frame)==1);
    assert(q.write_idx==w+1u);
    return &q.records[w%AUTO_CLOCK_RECORDS];
}
static uint32_t slot_state(uint32_t slot) {return q.slots[slot-FIRST];}
static void ack_head(void)
{
    const auto_wire_clock *r=auto_clock_peek(&q);
    assert(r);assert(auto_clock_ack(&q,r->id,1));
}

/* Thousands of frames at the 8-frame cadence plus block ends: every anchor is
 * valid, serials never wrap or repeat, slots cycle, storage stays bounded. */
static void sustained(void)
{
    uint32_t n=0;
    setup();
    for(uint64_t f=0;f<4096;f++) for(int end=0;end<2;end++) {
        if(end ? f%64!=63 : f%8!=0) continue;
        const auto_wire_clock *r=sample(f);
        n++;
        assert(r->flags==1 && r->id==n && r->serial==n && r->frame==f && r->owner==7);
        assert(r->slot==FIRST+(n-1)%AUTO_CLOCK_SLOTS);
        assert(r->raw==dev.reports[r->slot] && r->raw==r->repeated && r->label_first==n && r->label_second==n);
        assert(slot_state(r->slot)==AUTO_CLOCK_HELD);
        ack_head();
        assert(slot_state(r->slot)==AUTO_CLOCK_FREE);
    }
    assert(n==576 && q.attempted==576 && !q.lost && !q.quarantined && dev.emits==576);
    assert(q.read_idx==576 && q.write_idx==576 && !q.invalid && !q.saturated);
}

/* A timed-out anchor's late write lands on a quarantined slot; later anchors
 * stay valid, never reuse that slot, and a delayed label is not a completion. */
static void delayed_write(void)
{
    setup();
    const auto_wire_clock *r=sample(0);assert(r->flags==1 && r->slot==FIRST);ack_head();
    dev.hold=1;
    r=sample(8);assert(r->flags==2 && r->serial==2 && r->slot==FIRST+1 && !r->raw);
    ack_head();
    assert(slot_state(FIRST+1)==AUTO_CLOCK_QUARANTINED && q.quarantined==1);
    dev.hold=0; /* serial 2 now executes during serial 3's polling */
    r=sample(16);
    assert(r->flags==1 && r->serial==3 && r->slot==FIRST+2 && r->label_first==3 && r->polls>=2);
    ack_head();
    for(uint32_t i=0;i<200;i++) {
        r=sample(24+8*i);assert(r->flags==1 && r->slot!=FIRST+1);ack_head();
    }
    assert(slot_state(FIRST+1)==AUTO_CLOCK_QUARANTINED && q.quarantined==1 && !q.lost);
}

/* An unpublished anchor quarantines; the next PUT publishes both commands and
 * the label may skip the intermediate serial: still one exact completion. */
static void cumulative_skip(void)
{
    setup();
    const auto_wire_clock *r=sample(0);ack_head();
    dev.unpublished=1;
    r=sample(8);assert(r->flags==8 && r->serial==2 && !dev.fifo[1].published);ack_head();
    assert(slot_state(FIRST+1)==AUTO_CLOCK_QUARANTINED);
    dev.unpublished=0;dev.burst=1; /* both execute before the first label read of serial 3 */
    r=sample(16);
    assert(r->flags==1 && r->serial==3 && r->label_first==3 && r->polls==1);
    ack_head();
    assert(dev.label==3 && dev.head==dev.tail);
}

static void future_and_regressed(void)
{
    setup();
    dev.future=1;
    const auto_wire_clock *r=sample(0);assert(r->flags==8 && q.invalid);ack_head();
    assert(slot_state(FIRST)==AUTO_CLOCK_QUARANTINED);
    uint32_t emits=dev.emits;dev.future=0;
    r=sample(8);assert(r->flags==8 && dev.emits==emits);ack_head(); /* invalid: no device write */
    assert(slot_state(FIRST+1)==AUTO_CLOCK_FREE);
    setup();
    r=sample(0);ack_head();r=sample(8);ack_head();
    dev.regress=1;
    r=sample(16);assert(r->flags==8 && q.invalid);ack_head();
    assert(slot_state(FIRST+2)==AUTO_CLOCK_QUARANTINED);
}

/* A slow sink: records and slots stay held; later attempts are visible losses
 * with no device write; acceptance resumes sampling on reused slots. */
static void slow_and_refusing_sink(void)
{
    setup();
    for(uint32_t i=0;i<AUTO_CLOCK_SLOTS;i++) assert(sample(8*i)->flags==1);
    for(uint32_t i=0;i<1000;i++) assert(auto_clock_sample(&q,&port,7,1000+i)==2);
    assert(dev.emits==AUTO_CLOCK_SLOTS && q.lost==1000 && q.attempted==1016);
    for(uint32_t i=0;i<100;i++) {
        const auto_wire_clock *r=auto_clock_peek(&q);
        assert(r && r->id==1 && !auto_clock_ack(&q,1,0)); /* partial: nothing released */
    }
    for(uint32_t s=0;s<AUTO_CLOCK_SLOTS;s++) assert(q.slots[s]==AUTO_CLOCK_HELD);
    ack_head();
    assert(slot_state(FIRST)==AUTO_CLOCK_FREE);
    const auto_wire_clock *r=sample(5000);
    assert(r->flags==1 && r->serial==17 && r->slot==FIRST);
    assert(dev.emits==AUTO_CLOCK_SLOTS+1);
}

static void no_emission_and_stop(void)
{
    setup();
    dev.no_room=1;
    const auto_wire_clock *r=sample(0);assert(r->flags==8 && !dev.emits);ack_head();
    assert(slot_state(FIRST)==AUTO_CLOCK_FREE && !q.quarantined);
    dev.no_room=0;dev.live=0; /* STOP before emit */
    r=sample(8);assert(r->flags==8 && !dev.emits);
    assert(slot_state(r->slot)==AUTO_CLOCK_HELD); /* until acceptance */
    ack_head();assert(slot_state(FIRST+1)==AUTO_CLOCK_FREE);
    setup();
    dev.hold=1;dev.stop_at_label=3; /* STOP while polling an emitted anchor */
    r=sample(0);assert(r->flags==8 && dev.emits==1);ack_head();
    assert(slot_state(FIRST)==AUTO_CLOCK_QUARANTINED);
    setup();
    r=sample(0);assert(r->flags==1); /* STOP before acceptance */
    dev.live=0;
    r=sample(8);assert(r->flags==8 && dev.emits==1);
    ack_head();ack_head();
    assert(slot_state(FIRST)==AUTO_CLOCK_FREE && slot_state(FIRST+1)==AUTO_CLOCK_FREE);
}

static void serial_exhaustion(void)
{
    setup();
    q.serial=UINT32_MAX-2u;
    const auto_wire_clock *r=sample(0);assert(r->flags==1 && r->serial==UINT32_MAX-1u);ack_head();
    assert(auto_clock_sample(&q,&port,7,8)==2 && q.lost==1 && dev.emits==1);
}

static void all_quarantined(void)
{
    setup();
    dev.hold=1;
    for(uint32_t i=0;i<AUTO_CLOCK_SLOTS;i++) {assert(sample(8*i)->flags==2);ack_head();}
    assert(q.quarantined==AUTO_CLOCK_SLOTS);
    for(uint32_t i=0;i<100;i++) assert(auto_clock_sample(&q,&port,7,500+i)==2);
    assert(dev.emits==AUTO_CLOCK_SLOTS && q.lost==100);
}

static volatile int produced_all;
static void *producer(void *u)
{
    (void)u;
    for(uint32_t i=0;i<2048;i++) {
        uint32_t spins=0;
        while(!auto_clock_room(&q)) {assert(++spins<100000000u);sched_yield();}
        assert(auto_clock_sample(&q,&port,7,i)==1);
    }
    return NULL;
}
static void *consumer(void *u)
{
    (void)u;
    for(uint32_t i=0;i<2048;i++) {
        const auto_wire_clock *r;
        uint32_t spins=0;
        while(!(r=auto_clock_peek(&q))) {assert(++spins<100000000u);sched_yield();}
        assert(r->id==i+1 && r->serial==i+1 && r->flags==1 && r->raw==r->repeated && r->raw);
        assert(auto_clock_ack(&q,i+1,1));
    }
    return NULL;
}
static void threaded(void)
{
    pthread_t p,c;
    setup();
    assert(!pthread_create(&p,NULL,producer,NULL));assert(!pthread_create(&c,NULL,consumer,NULL));
    assert(!pthread_join(p,NULL));assert(!pthread_join(c,NULL));
    assert(q.attempted==2048 && !q.lost && !q.quarantined && dev.emits==2048);
}

int main(void)
{
    sustained();delayed_write();cumulative_skip();future_and_regressed();
    slow_and_refusing_sink();no_emission_and_stop();serial_exhaustion();all_quarantined();threaded();
    puts("AUTO clock slot reuse: sustained cadence, quarantine, cumulative labels, sinks and STOP PASS");
}
