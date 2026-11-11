#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auto_completion.h"

static auto_publication publication;
static unsigned labels_read, timers_read, barriers;
static uint32_t labels[2];
static uint64_t timers[4];
static uint32_t label(void *p) { (void)p;return labels[labels_read++]; }
static uint64_t timer(void *p,uint32_t slot) {
    (void)p;assert(slot==19u+(timers_read&1u));return timers[timers_read++];
}
static void sync_device(void *p) { (void)p;barriers++; }
static auto_completion_port port={NULL,label,timer,sync_device};
static void values(uint32_t a,uint32_t b,uint64_t c,uint64_t d) {
    labels[0]=a;labels[1]=b;timers[0]=timers[2]=c;timers[1]=timers[3]=d;
    labels_read=timers_read=barriers=0;
}
enum { N=20000 };
static uint32_t payload[N];
static void *producer(void *unused) {
    (void)unused;
    for(uint32_t i=1;i<=N;i++) {
        payload[i-1]=i;
        assert(auto_publication_commit(&publication,i*2));
        uint32_t cut=auto_publication_before(&publication);
        assert(cut==i*2);
        assert(auto_publication_after(&publication,cut));
    }
    return NULL;
}
static void *consumer(void *unused) {
    (void)unused;uint32_t read=0;
    while(read<N) {
        uint32_t cut=auto_publication_read(&publication);
        assert(cut<=N*2 && !(cut&1));
        while(read<cut/2) {assert(payload[read]==read+1);read++;}
    }
    return NULL;
}
int main(void) {
    auto_completion state={0};auto_completion_fact fact;
    assert(!auto_publication_before(&publication));
    assert(!auto_publication_after(&publication,2));
    assert(!auto_publication_commit(&publication,1));
    assert(auto_publication_commit(&publication,2));
    assert(auto_publication_before(&publication)==2 && !auto_publication_read(&publication));
    values(2,2,100,120);
    assert(auto_completion_read(&state,&publication,&port,19,20,1,2,&fact)==AUTO_COMPLETION_DEFER);
    assert(!timers_read && !fact.raw_begin && !fact.raw_end);
    assert(auto_publication_after(&publication,2));
    assert(!auto_publication_commit(&publication,2));
    assert(!auto_publication_commit(&publication,0));
    assert(!auto_publication_after(&publication,4));
    values(1,1,100,120);
    assert(auto_completion_read(&state,&publication,&port,19,20,1,2,&fact)==AUTO_COMPLETION_PENDING);
    assert(!timers_read);
    values(2,2,100,120);
    assert(auto_completion_read(&state,&publication,&port,19,20,1,2,&fact)==AUTO_COMPLETION_READY);
    assert(fact.raw_begin==100 && fact.raw_end==120 && fact.published==2 && timers_read==4 && barriers>=2);
    assert(auto_publication_commit(&publication,8));assert(auto_publication_after(&publication,8));
    assert(!auto_publication_after(&publication,2));
    values(8,8,200,230); /* missed intermediate labels are legitimate */
    assert(auto_completion_read(&state,&publication,&port,19,20,3,4,&fact)==AUTO_COMPLETION_READY);
    assert(fact.published==8 && fact.raw_begin==200);
    values(8,10,200,230); /* progress outran the acquired publication snapshot */
    assert(auto_completion_read(&state,&publication,&port,19,20,5,6,&fact)==AUTO_COMPLETION_DEFER);
    assert(!fact.raw_begin && !fact.raw_end);
    values(8,8,200,230);timers[2]=201;
    assert(auto_completion_read(&state,&publication,&port,19,20,5,6,&fact)==AUTO_COMPLETION_REFUSED);
    values(8,8,UINT64_MAX,UINT64_MAX);
    assert(auto_completion_read(&state,&publication,&port,19,20,5,6,&fact)==AUTO_COMPLETION_REFUSED);
    values(8,8,230,200);
    assert(auto_completion_read(&state,&publication,&port,19,20,5,6,&fact)==AUTO_COMPLETION_REFUSED);
    values(0,0,200,230);
    assert(auto_completion_read(&state,&publication,&port,19,20,5,6,&fact)==AUTO_COMPLETION_REFUSED);
    assert(state.invalid); /* a later good label cannot erase regression */
    values(8,8,200,230);
    assert(auto_completion_read(&state,&publication,&port,19,20,5,6,&fact)==AUTO_COMPLETION_REFUSED && !labels_read);
    memset(&state,0,sizeof state);
    assert(auto_completion_read(&state,&publication,&port,19,20,UINT32_MAX,0,&fact)==AUTO_COMPLETION_REFUSED);
    assert(!labels_read);
    memset(&publication,0,sizeof publication);
    pthread_t a,b;
    assert(!pthread_create(&a,NULL,producer,NULL));assert(!pthread_create(&b,NULL,consumer,NULL));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    puts("AUTO cumulative completion and 20000 metadata/publication handoffs PASS");
}
