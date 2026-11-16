#include "auto_ring.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

static void generations_and_sink(void)
{
    auto_ring r;
    auto_ring_slot slots[3];
    auto_ring_ticket t;
    assert(auto_ring_init(&r, slots, 3, 2, 2));
    assert(auto_ring_admit(&r, 1, 7, 1, &t)==AUTO_RING_OK);
    assert(t.generation==1 && t.local==0 && t.slot==0 && t.pair==0);
    assert(auto_ring_admit(&r, 1, 7, 1, &t)==AUTO_RING_OK && t.pair==1);
    assert(auto_ring_seal(&r, 1));
    assert(auto_ring_admit(&r, 2, 7, 1, &t)==AUTO_RING_OK && t.local==1);
    assert(auto_ring_seal(&r, 1));
    /* Local generation zero can be scheduled again without a PPU read. The
     * emitter must put the copy-complete GPU wait before this generation. */
    assert(auto_ring_admit(&r, 3, 7, 1, &t)==AUTO_RING_OK && t.local==0 && t.slot==2);
    assert(auto_ring_seal(&r, 1));
    assert(auto_ring_admit(&r, 4, 7, 1, &t)==AUTO_RING_FULL);
    assert(auto_ring_ready(&r, 1, 0, 6)==AUTO_RING_PENDING);
    assert(auto_ring_ready(&r, 1, 2, 6)==AUTO_RING_OK);
    assert(!auto_ring_ack(&r, 1, 0, 0)); /* short sink cannot free anything */
    assert(auto_ring_admit(&r, 4, 7, 1, &t)==AUTO_RING_FULL);
    assert(auto_ring_ack(&r, 1, 0, 1));
    assert(auto_ring_admit(&r, 4, 7, 1, &t)==AUTO_RING_FULL); /* one record remains */
    assert(!auto_ring_ack(&r, 1, 0, 1)); /* duplicate */
    assert(auto_ring_ack(&r, 1, 1, 1));
    assert(auto_ring_admit(&r, 4, 7, 1, &t)==AUTO_RING_OK && t.generation==4 && t.slot==0);
}

static void refusal_and_quarantine(void)
{
    auto_ring r;
    auto_ring_slot slots[3];
    auto_ring_ticket t;
    assert(!auto_ring_init(&r, slots, 3, 1, 2));
    assert(auto_ring_init(&r, slots, 3, 2, 2));
    assert(auto_ring_admit(&r, 1, 1, 0, &t)==AUTO_RING_METADATA);
    assert(r.issued==0); /* reserve metadata before a destination/generation */
    assert(auto_ring_admit(&r, 1, 1, 1, &t)==AUTO_RING_OK);
    assert(auto_ring_admit(&r, 2, 1, 1, &t)==AUTO_RING_BOUNDARY);
    assert(auto_ring_admit(&r, 1, 2, 1, &t)==AUTO_RING_BOUNDARY);
    assert(auto_ring_ready(&r, 1, 2, 2)==AUTO_RING_PENDING); /* still open */
    assert(auto_ring_seal(&r, 0)); /* uncertain emission */
    assert(auto_ring_ready(&r, 1, 2, 2)==AUTO_RING_INVALID);
    assert(!auto_ring_ack(&r, 1, 0, 1));
    assert(auto_ring_admit(&r, 1, 1, 1, &t)==AUTO_RING_INVALID);

    assert(auto_ring_init(&r, slots, 3, 2, 2));
    assert(auto_ring_admit(&r, 1, 1, 1, &t)==AUTO_RING_OK);
    assert(auto_ring_seal(&r, 1));
    assert(auto_ring_ready(&r, 1, 4, 2)==AUTO_RING_INVALID); /* future */
    assert(!auto_ring_ack(&r, 1, 0, 1));
    assert(auto_ring_init(&r, slots, 3, 2, 2));
    assert(auto_ring_admit(&r, 1, 1, 1, &t)==AUTO_RING_OK);
    assert(auto_ring_seal(&r, 1));
    assert(auto_ring_ready(&r, 1, 2, 0)==AUTO_RING_PENDING); /* PUT/store race */
    assert(auto_ring_ready(&r, 1, 2, 2)==AUTO_RING_OK);
    assert(auto_ring_ready(&r, 1, 1, 2)==AUTO_RING_INVALID); /* regression */
    assert(auto_ring_init(&r, slots, 3, 2, 2));
    r.issued=AUTO_RING_GENERATION_MAX;
    r.retired=AUTO_RING_GENERATION_MAX;
    assert(auto_ring_admit(&r, 1, 1, 1, &t)==AUTO_RING_SERIAL);
}

typedef struct concurrent { auto_ring r; auto_ring_slot slots[3]; uint32_t label; } concurrent;
static void *producer(void *arg)
{
    concurrent *c=arg;
    for(uint32_t seq=1;seq<=4000;seq++) {
        for(uint32_t i=0;i<7;i++) {
            auto_ring_ticket t;
            int rc;
            while((rc=auto_ring_admit(&c->r,seq/10,1,1,&t))==AUTO_RING_FULL) sched_yield();
            assert(rc==AUTO_RING_OK && t.generation==seq && t.pair==i);
        }
        assert(auto_ring_seal(&c->r,1));
        __atomic_store_n(&c->label,2u*seq,__ATOMIC_RELEASE);
    }
    return NULL;
}
static void *consumer(void *arg)
{
    concurrent *c=arg;
    for(uint32_t seq=1;seq<=4000;seq++) {
        while(__atomic_load_n(&c->r.issued,__ATOMIC_ACQUIRE)<seq) sched_yield();
        for(;;) {
            uint32_t label=__atomic_load_n(&c->label,__ATOMIC_ACQUIRE);
            uint32_t frontier=2u*__atomic_load_n(&c->r.issued,__ATOMIC_ACQUIRE);
            int rc=auto_ring_ready(&c->r,seq,label,frontier);
            assert(rc==AUTO_RING_PENDING || rc==AUTO_RING_OK);
            if(rc==AUTO_RING_OK)break;
            sched_yield();
        }
        for(uint32_t i=0;i<7;i++) {
            assert(!auto_ring_ack(&c->r,seq,i,0));
            assert(auto_ring_ack(&c->r,seq,i,1));
        }
    }
    return NULL;
}
static void concurrent_wraps(void)
{
    concurrent c={0};pthread_t a,b;
    assert(auto_ring_init(&c.r,c.slots,3,2,7));
    assert(!pthread_create(&a,NULL,producer,&c));assert(!pthread_create(&b,NULL,consumer,&c));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    assert(c.r.retired==4000 && !c.r.invalid);
}

int main(void)
{
    generations_and_sink();
    refusal_and_quarantine();
    concurrent_wraps();
    puts("auto-ring core: generations, destination ownership, refusal and quarantine PASS");
    return 0;
}
