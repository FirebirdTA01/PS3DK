/* Runtime queue controls. Device completion is supplied by the consumer test;
 * these controls make no claim about RSX memory or report ordering. */
#include "auto_queue.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static auto_queue q;
static auto_queue_entry entries[4];

static void bounded(void)
{
    auto_queue_entry *e;
    const auto_queue_entry *seen;
    assert(auto_queue_init(&q,entries,4,2));
    e=auto_queue_claim(&q); assert(e); e->op.args[0]=73;
    assert(!auto_queue_peek(&q)); /* not visible before commit */
    assert(!auto_queue_claim(&q)); /* same-owner reentry cannot steal a slot */
    assert(q.refusal==AUTO_QUEUE_REENTRY);
    auto_queue_commit(&q);
    seen=auto_queue_peek(&q); assert(seen && seen->op.id==1 && seen->op.args[0]==73);
    assert(!auto_queue_ack(&q,1,1,0)); /* partial sink does not release either resource */
    assert(!auto_queue_ack(&q,99,1,1)); /* acceptance belongs to this exact record */
    assert(auto_queue_peek(&q)==seen);
    e=auto_queue_claim(&q); assert(e); auto_queue_commit(&q);
    assert(!auto_queue_claim(&q));
    assert(q.refusal==AUTO_QUEUE_REPORTS);
    assert(auto_queue_ack(&q,1,1,1));
    e=auto_queue_claim(&q); assert(e && e->op.id==3 && e->op.serial_begin==5);
    auto_queue_commit(&q);
    assert(auto_queue_ack(&q,2,0,1)); /* incomplete metadata accepted, physical pair quarantined */
    assert(auto_queue_ack(&q,3,1,1));
    assert(q.written==3 && auto_queue_held(&q)==1);
    e=auto_queue_claim(&q); assert(e); auto_queue_commit(&q);
    assert(!auto_queue_claim(&q)); /* quarantine still charges capacity */
    auto_queue_stop(&q);
    assert(auto_queue_ack(&q,4,1,1)); /* a supplied completion cannot free after invalidation */
    assert(auto_queue_held(&q)==2 && !auto_queue_claim(&q));
    assert(q.refusal==AUTO_QUEUE_STOPPED);
}

static void limits(void)
{
    assert(!auto_queue_init(&q,entries,0,1));
    assert(!auto_queue_init(&q,entries,4,0));
    assert(!auto_queue_init(&q,entries,1025,1));
    assert(!auto_queue_init(&q,entries,4,505));
    assert(auto_queue_init(&q,entries,1,1));
    q.next_serial=UINT32_MAX;
    assert(!auto_queue_claim(&q));
    assert(q.refusal==AUTO_QUEUE_EXHAUSTED);
    assert(auto_queue_held(&q)==0);
    q.next_serial=1; q.next_id=UINT64_MAX;
    assert(!auto_queue_claim(&q));
    assert(q.refusal==AUTO_QUEUE_EXHAUSTED);
    assert(auto_queue_held(&q)==0);
    assert(auto_queue_init(&q,entries,1,2));
    assert(auto_queue_claim(&q));auto_queue_commit(&q);
    assert(!auto_queue_claim(&q) && q.refusal==AUTO_QUEUE_METADATA);
}

static void *producer(void *unused)
{
    unsigned i;
    (void)unused;
    for(i=1;i<=20000;i++) {
        auto_queue_entry *e;
        while(!(e=auto_queue_claim(&q))) sched_yield();
        e->op.tb_before=i; e->op.tb_after=i+1;
        e->op.args[0]=i^0xabcdeu;
        auto_queue_commit(&q);
    }
    return NULL;
}

static void *consumer(void *unused)
{
    unsigned i;
    (void)unused;
    for(i=1;i<=20000;i++) {
        const auto_queue_entry *e;
        while(!(e=auto_queue_peek(&q))) sched_yield();
        assert(e->op.id==i && e->op.tb_before==i && e->op.tb_after==i+1);
        assert(e->op.args[0]==(i^0xabcdeu));
        assert(e->op.serial_end==i*2);
        assert(auto_queue_ack(&q,i,1,1));
    }
    return NULL;
}

int main(void)
{
    pthread_t a,b;
    bounded(); limits();
    assert(auto_queue_init(&q,entries,4,2));
    assert(!pthread_create(&a,NULL,producer,NULL));
    assert(!pthread_create(&b,NULL,consumer,NULL));
    assert(!pthread_join(a,NULL)); assert(!pthread_join(b,NULL));
    assert(q.written==20000 && auto_queue_held(&q)==0 && !auto_queue_peek(&q));
    puts("AUTO queue: bounds, short sink, quarantine, stop, serials and 20000 handoffs pass");
    return 0;
}
