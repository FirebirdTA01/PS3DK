/* Bounded C11 model of one wait ring slot published twice and consumed twice.
 * Mirrors payload -> head -> payload read -> tail -> slot reuse. This is not
 * the runtime, STOP protocol, target compiler, or a hardware memory model.
 * Assumptions select the exercised path; a successful run must explore at
 * least one complete execution. No loop or scheduling synchronization is added.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <genmc.h>

#ifndef HEAD_STORE
#define HEAD_STORE memory_order_release
#endif
#ifndef HEAD_LOAD
#define HEAD_LOAD memory_order_acquire
#endif
#ifndef TAIL_STORE
#define TAIL_STORE memory_order_release
#endif
#ifndef TAIL_LOAD
#define TAIL_LOAD memory_order_acquire
#endif

static _Atomic uint64_t head, tail;
static uint64_t payload;
static void *produce(void *unused)
{
    (void)unused;
    payload=1;
    atomic_store_explicit(&head,1,HEAD_STORE);
    __VERIFIER_assume(atomic_load_explicit(&tail,TAIL_LOAD)==1);
    payload=2;
    atomic_store_explicit(&head,2,HEAD_STORE);
    return NULL;
}
static void *consume(void *unused)
{
    (void)unused;
    __VERIFIER_assume(atomic_load_explicit(&head,HEAD_LOAD)==1);
    assert(payload==1);
    atomic_store_explicit(&tail,1,TAIL_STORE);
    __VERIFIER_assume(atomic_load_explicit(&head,HEAD_LOAD)==2);
    assert(payload==2);
    atomic_store_explicit(&tail,2,TAIL_STORE);
    return NULL;
}
int main(void)
{
    pthread_t p,c;
    if(pthread_create(&p,NULL,produce,NULL) || pthread_create(&c,NULL,consume,NULL))abort();
    if(pthread_join(p,NULL) || pthread_join(c,NULL))abort();
    return 0;
}
