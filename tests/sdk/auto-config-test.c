#include "auto_config.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static auto_config_store store;
static uint32_t entered, resume;
static auto_wire_config candidate(void) {
    auto_wire_config c;
    memset(&c,0,sizeof c);
    c.instance=1; c.domain=1026; c.epoch=1; c.owner=7; c.generation=1;
    c.report_first=1024; c.report_count=1024; c.op_label=239; c.clock_label=238;
    c.pairs=504; c.metadata=1024; c.frames=256; c.allocated=500000; c.ceiling=2097152;
    return c;
}
static void initialize(void) {
    const uint8_t digest[32]={42}, binding[32]={99};
    auto_config_init(&store,79800000,digest,binding);
}
static void controls(void) {
    auto_wire_config c=candidate();
    const auto_wire_config *view;
    uint32_t ticket;
    initialize();
    assert(!auto_config_read(&store));
    assert(!auto_config_can_admit(&store));
    assert(auto_config_freeze(&store));
    view=auto_config_read(&store);
    assert(view && view->reason==AUTO_NO_GCM_INIT && !view->owner && !view->domain);
    assert(!auto_config_begin(&store));
    assert(!auto_config_can_admit(&store));
    initialize();
    ticket=auto_config_begin(&store); assert(ticket);
    assert(!auto_config_begin(&store));
    assert(!auto_config_freeze(&store));
    assert(!auto_config_read(&store));
    assert(auto_config_commit(&store,ticket,&c));
    assert(!auto_config_read(&store)); /* drain cannot freeze an override window */
    ticket=auto_config_begin(&store); assert(ticket);
    auto_config_abort(&store,ticket); /* invalid optional override preserves defaults */
    ticket=auto_config_begin(&store); assert(ticket);
    c.report_first=100; c.report_count=20; c.pairs=2;
    assert(auto_config_commit(&store,ticket,&c));
    assert(auto_config_freeze(&store));
    view=auto_config_read(&store);
    assert(view && !view->reason && view->report_first==100 && view->pairs==2);
    assert(view->frequency==79800000 && view->mechanism[0]==42 && view->binding[0]==99);
    assert(auto_config_can_admit(&store));
    assert(!auto_config_begin(&store));
    auto_config_invalidate(&store,AUTO_STATUS_CONFLICT);
    assert(!auto_config_can_admit(&store));
    assert(auto_config_read(&store)==view && !view->reason && view->report_first==100);
    auto_config_invalidate(&store,AUTO_STATUS_RESET);
    assert(auto_config_flags(&store)==(AUTO_STATUS_CONFLICT|AUTO_STATUS_RESET));
    assert(!auto_config_begin(&store));
    initialize();
    auto_config_stop(&store);
    view=auto_config_read(&store);
    assert(view && view->reason==AUTO_NO_GCM_INIT);
    assert(!auto_config_begin(&store));
    for(uint32_t reason=1;reason<=10;reason++) {
        initialize(); c=candidate(); c.reason=reason; c.application_rc=-7;
        ticket=auto_config_begin(&store); assert(ticket);
        assert(auto_config_commit(&store,ticket,&c));
        assert(auto_config_freeze(&store));
        view=auto_config_read(&store);
        assert(view && view->reason==reason && view->application_rc==-7);
        assert(!view->owner && !view->domain && !view->allocated && !view->pairs);
        assert(!auto_config_can_admit(&store));
    }
    initialize(); c=candidate();
    ticket=auto_config_begin(&store); assert(ticket);
    assert(auto_config_commit(&store,ticket,&c));
    ticket=auto_config_begin(&store); assert(ticket);
    c.reason=11;
    assert(!auto_config_commit(&store,ticket,&c));
    assert(auto_config_freeze(&store));
    assert(auto_config_read(&store)->reason==0); /* bad replacement preserved */
}
static void *configurer(void *unused) {
    auto_wire_config c=candidate();
    uint32_t ticket=auto_config_begin(&store);
    (void)unused;
    assert(ticket);
    __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&resume,__ATOMIC_ACQUIRE)) sched_yield();
    assert(!auto_config_commit(&store,ticket,&c));
    return NULL;
}
static void seal_race(void) {
    pthread_t owner;
    initialize(); entered=resume=0;
    assert(!pthread_create(&owner,NULL,configurer,NULL));
    while(!__atomic_load_n(&entered,__ATOMIC_ACQUIRE)) sched_yield();
    auto_config_stop(&store);
    assert(!auto_config_read(&store)); /* writer still owns the mutable bytes */
    assert(!auto_config_can_admit(&store));
    assert(!auto_config_begin(&store));
    __atomic_store_n(&resume,1,__ATOMIC_RELEASE);
    assert(!pthread_join(owner,NULL));
    assert(auto_config_read(&store)->reason==AUTO_STOPPED);
    assert(!auto_config_read(&store)->owner && !auto_config_read(&store)->domain);
}
int main(void) {
    controls();
    for(unsigned i=0;i<1000;i++) seal_race();
    puts("AUTO config freeze/refusal/invalidation + 1000 attach/seal handoffs PASS");
    return 0;
}
