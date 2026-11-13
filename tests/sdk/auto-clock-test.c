/* Host supplied device facts, not evidence of RSX ordering or timer rate. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auto_clock.h"

static auto_clock clock_state;
static struct {
    uint64_t tick,raw;
    unsigned mode,ticks,reads,emits,flushes,serial;
} fake;
enum { VALID, LATE, REGRESSED, UNSTABLE, POISON, FUTURE, NEVER, UNPUBLISHED,
       NO_ROOM, LATE_READ, INVALIDATED };
static uint64_t tb(void *unused) {
    (void)unused;
    fake.ticks++;
    if(fake.mode==REGRESSED && fake.ticks==2) return fake.tick-2;
    if(fake.mode==LATE && fake.ticks==2) fake.tick+=200;
    if(fake.mode==NEVER) return fake.tick;
    return fake.tick++;
}
static int live(void *unused) {(void)unused;return fake.mode!=INVALIDATED || fake.ticks<2;}
static int emit(void *unused,uint32_t slot,uint32_t serial) {
    (void)unused;
    assert(slot==1024+(serial-1)%16); /* round-robin reuse of sixteen physical slots */
    if(fake.mode==NO_ROOM) return 0;
    fake.emits++;fake.serial=serial;return 1;
}
static void flush(void *unused) {
    (void)unused;fake.flushes++;
    uint32_t serial=auto_clock_before(&clock_state);
    assert(serial==fake.serial);
    if(fake.mode!=UNPUBLISHED) assert(auto_clock_after(&clock_state,serial));
}
static uint32_t label(void *unused) {
    (void)unused;
    return fake.mode==FUTURE ? fake.serial+1:fake.mode==NEVER ? 0:fake.serial;
}
static uint64_t timer(void *unused,uint32_t slot) {
    (void)unused;assert(slot==1024+(fake.serial-1)%16);fake.reads++;
    if(fake.mode==LATE_READ) fake.tick+=200;
    return fake.mode==POISON ? UINT64_MAX:fake.raw+(fake.mode==UNSTABLE ? fake.reads:0);
}
static void sync_device(void *unused) {(void)unused;}
static const auto_clock_port port={NULL,tb,live,emit,flush,label,timer,sync_device};
static void setup(unsigned mode) {
    assert(auto_clock_init(&clock_state,1,1,1024,100));
    memset(&fake,0,sizeof fake);fake.tick=100;fake.raw=9000;fake.mode=mode;
}
static void sample(unsigned mode,unsigned flags) {
    setup(mode);
    assert(auto_clock_sample(&clock_state,&port,42,0)==1);
    const auto_wire_clock *r=auto_clock_peek(&clock_state);
    assert(r && r->flags==flags && r->owner==42 && r->frame==0);
    assert(r->instance==1 && r->epoch==1 && r->id==1 && r->serial==1 && r->slot==1024);
    assert(clock_state.attempted==1 && !clock_state.lost);
    if(mode==VALID) {
        assert(r->raw==9000 && r->repeated==9000 && r->label_first==1 && r->label_second==1);
        assert(r->tb_after>=r->tb_before && r->elapsed==r->tb_after-r->tb_before);
    } else assert(!r->raw && !r->repeated);
    if(mode==LATE || mode==REGRESSED || mode==FUTURE || mode==UNPUBLISHED || mode==NO_ROOM)
        assert(!fake.reads);
    if(mode==NO_ROOM) assert(!fake.emits && !fake.flushes);
    if(mode==NEVER) assert(r->polls==100000);
    assert(!auto_clock_ack(&clock_state,1,0));assert(auto_clock_peek(&clock_state)==r);
    assert(!auto_clock_ack(&clock_state,2,1));assert(auto_clock_ack(&clock_state,1,1));
    assert(!auto_clock_peek(&clock_state));
}
static void *produce(void *unused) {
    (void)unused;
    for(unsigned i=0;i<16;i++) {fake.raw=1000+i;assert(auto_clock_sample(&clock_state,&port,42,i)==1);}
    return NULL;
}
static void *consume(void *unused) {
    (void)unused;
    for(unsigned i=0;i<16;i++) {
        const auto_wire_clock *r;
        while(!(r=auto_clock_peek(&clock_state))) {}
        assert(r->id==i+1 && r->serial==i+1 && r->slot==1024+i && r->flags==1 && r->raw==1000+i);
        assert(auto_clock_ack(&clock_state,i+1,1));
    }
    return NULL;
}
int main(void) {
    sample(VALID,1);sample(LATE,2);sample(REGRESSED,4);sample(UNSTABLE,8);
    sample(POISON,8);sample(FUTURE,8);sample(NEVER,2);sample(UNPUBLISHED,8);
    sample(NO_ROOM,8);sample(LATE_READ,2);sample(INVALIDATED,8);
    setup(VALID);
    assert(auto_clock_sample(&clock_state,&port,42,0)==1);
    assert(auto_clock_ack(&clock_state,1,1));
    fake.mode=NEVER; /* a completed serial 1 must not regress to physical zero */
    assert(auto_clock_sample(&clock_state,&port,42,1)==1);
    const auto_wire_clock *regressed=auto_clock_peek(&clock_state);
    assert(regressed->flags==8 && regressed->polls==1);
    setup(VALID);
    assert(!auto_clock_after(&clock_state,1));
    pthread_t p,c;assert(!pthread_create(&p,NULL,produce,NULL));assert(!pthread_create(&c,NULL,consume,NULL));
    assert(!pthread_join(p,NULL));assert(!pthread_join(c,NULL));
    assert(clock_state.attempted==16 && !clock_state.lost && fake.emits==16);
    /* After whole acceptance of valid records the slots return: serial 17
     * reuses the first slot. Serial exhaustion is a loss with no write. */
    fake.raw=2000;
    assert(auto_clock_sample(&clock_state,&port,42,16)==1);
    const auto_wire_clock *reused=auto_clock_peek(&clock_state);
    assert(reused->flags==1 && reused->id==17 && reused->serial==17 && reused->slot==1024 && reused->raw==2000);
    assert(clock_state.attempted==17 && !clock_state.lost && fake.emits==17);
    assert(auto_clock_ack(&clock_state,17,1));
    clock_state.serial=UINT32_MAX-1u;
    assert(auto_clock_sample(&clock_state,&port,42,17)==2 && clock_state.lost==1 && fake.emits==17);
    clock_state.attempted=clock_state.lost=UINT64_MAX;
    assert(auto_clock_sample(&clock_state,&port,42,18)==2 && clock_state.saturated);
    puts("AUTO clock deadline, publication, stable identity, slot reuse and handoff PASS");
}
