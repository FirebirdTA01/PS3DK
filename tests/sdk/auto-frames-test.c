#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "auto_frames.h"

static auto_frames frames;
static auto_wire_frame rows[64];
enum { N=20000 };
static void *producer(void *unused) {
    (void)unused;
    for(uint64_t i=0;i<N;i++) {
        while(__atomic_load_n(&frames.write_idx,__ATOMIC_ACQUIRE)-
              __atomic_load_n(&frames.read_idx,__ATOMIC_ACQUIRE)==frames.capacity) {}
        assert(auto_frames_open(&frames,i,i*10));
        assert(auto_frames_count(&frames,1,0));assert(auto_frames_count(&frames,1,1));
        assert(auto_frames_close(&frames,42,i,i*10+5,0)==1);
    }
    return NULL;
}
static void *consumer(void *unused) {
    (void)unused;
    for(uint64_t i=0;i<N;i++) {
        const auto_wire_frame *f;
        while(!(f=auto_frames_peek(&frames))) {}
        assert(f->frame==i && f->owner==42 && f->flags==3);
        assert(f->counts[0][0]==1 && f->counts[0][1]==1 && f->tb_end-f->tb_begin==5);
        assert(auto_frames_ack(&frames,42,i,1));
    }
    return NULL;
}
int main(void) {
    assert(auto_frames_init(&frames,rows,2,1,42));
    assert(!auto_frames_count(&frames,1,0));
    assert(auto_frames_open(&frames,0,10));
    assert(!auto_frames_open(&frames,0,11));
    assert(auto_frames_count(&frames,1,0));
    assert(!auto_frames_close(&frames,42,0,20,0)); /* incomplete accounting cannot certify a frame */
    assert(auto_frames_count(&frames,1,2));
    assert(!auto_frames_close(&frames,41,0,20,0));
    assert(auto_frames_close(&frames,42,0,20,0)==1);
    assert(!auto_frames_close(&frames,42,0,20,0));
    const auto_wire_frame *f=auto_frames_peek(&frames);
    assert(f && f->frame==0 && f->counts[0][2]==1 && f->flags==3);
    assert(!auto_frames_ack(&frames,42,0,0));assert(auto_frames_peek(&frames)==f);
    assert(!auto_frames_ack(&frames,42,1,1));
    assert(auto_frames_open(&frames,1,30));
    assert(auto_frames_open(&frames,2,40)); /* previous frame lacked explicit closure */
    assert(auto_frames_close(&frames,42,2,50,0)==2); /* full queue, visible loss */
    assert(frames.lost==1 && !frames.active);
    assert(auto_frames_ack(&frames,42,0,1));
    f=auto_frames_peek(&frames);assert(f->frame==1 && f->flags==19 && f->tb_end==40);
    assert(auto_frames_ack(&frames,42,1,1));assert(!auto_frames_peek(&frames));
    assert(auto_frames_open(&frames,3,60));
    assert(auto_frames_close(&frames,42,3,55,0)==1); /* preserve regressed clock, no exact interval */
    f=auto_frames_peek(&frames);assert(f->frame==3 && f->flags==27 && f->lost==1 && f->tb_end==55);
    assert(auto_frames_ack(&frames,42,3,1));
    assert(auto_frames_open(&frames,4,70));
    frames.live.counts[1][0]=frames.live.counts[1][1]=UINT64_MAX;
    assert(auto_frames_count(&frames,2,0));assert(auto_frames_count(&frames,2,1));
    assert(auto_frames_close(&frames,42,4,80,0)==1);
    f=auto_frames_peek(&frames);assert(f->flags&4);assert(frames.saturated);
    assert(auto_frames_ack(&frames,42,4,1));
    assert(!auto_frames_open(&frames,UINT64_MAX,90));
    assert(auto_frames_init(&frames,rows,64,1,42));
    pthread_t a,b;assert(!pthread_create(&a,NULL,producer,NULL));assert(!pthread_create(&b,NULL,consumer,NULL));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));assert(!frames.lost);
    puts("AUTO frame identity, closure, loss, saturation and 20000 handoffs PASS");
}
