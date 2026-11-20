#include "auto_wire.h"
#include "auto_queue.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static uint32_t u32(const unsigned char *b) {return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
int main(void)
{
    unsigned char b[1024];auto_wire_config c={0};auto_wire_op op={0};auto_wire_frame f={0};
    c.instance=1;
    assert(auto_encode_config(b,sizeof b,&c)==176 && u32(b+24)==3071 && u32(b+60)==160);
    c.ring_bytes=16777216;c.ring_blocks=2;c.ring_pairs=126;c.ring_first=1544;c.ring_label=245;
    c.ring_stride=4096;c.ring_slots=4096;c.metadata_stride=sizeof(auto_queue_entry);c.legacy_pairs=252;
    assert(auto_encode_config(b,sizeof b,&c)==216 && u32(b+24)==4095 && u32(b+60)==176);
    assert(u32(b+176)==16777216 && u32(b+180)==2 && u32(b+184)==126 && u32(b+208)==252);
    memset(b,0xa5,sizeof b);assert(!auto_encode_config(b,215,&c) && b[0]==0xa5);
    op.flags=AUTO_OP_RING;op.ring_generation=3;op.ring_slot=2;op.ring_ordinal=9;op.ring_local=0;
    assert(auto_encode_ops_ring(b,sizeof b,1,&op,1)==192 && u32(b+12)==176);
    assert(u32(b+16+160)==3 && u32(b+16+164)==2 && u32(b+16+168)==9);
    assert(auto_encode_ops(b,sizeof b,1,&op,1)==176 && u32(b+12)==160);
    f.ring_full[0]=3;f.ring_state[2]=4;f.ranges_refused=7;f.range_refused[0]=5;f.range_refused[9]=2;
    memset(b,0xa5,sizeof b);
    assert(auto_encode_frame_ring(b,sizeof b,&f)==592 && u32(b+480)==3 && u32(b+520)==4 && u32(b+440)==7);
    /* the range tail at 528: reason r at slot r-1, the six slots past the ten reasons zero */
    assert(u32(b+528)==5 && u32(b+528+36)==2);
    for(unsigned i=1;i<16;i++) if(i!=9) assert(u32(b+528+4*i)==0);
    memset(b,0xa5,sizeof b);
    assert(auto_encode_frame(b,sizeof b,&f)==544 && u32(b+480)==5 && u32(b+480+36)==2);
    for(unsigned i=1;i<16;i++) if(i!=9) assert(u32(b+480+4*i)==0);
    assert(!auto_encode_frame(b,543,&f) && !auto_encode_frame_ring(b,591,&f));
    uint64_t counters[15]={0},ring[8]={7,3,14,4,224,4,3,99},ranges[10]={1,2,3,4,5,6,7,8,9,10};
    uint32_t reject[3]={0x00042000u,0x1234u,8};
    memset(b,0xa5,sizeof b);
    assert(auto_encode_status_ring(b,sizeof b,1,0,counters,ring,reject,ranges)==336);
    for(unsigned i=0;i<16;i++) assert(u32(b+208+8*i)==(i<10 ? i+1:0) && u32(b+212+8*i)==0);
    assert(u32(b+128)==7 && u32(b+152)==4 && u32(b+184)==99);
    assert(u32(b+192)==0x00042000u && u32(b+196)==0x1234u && u32(b+200)==8 && u32(b+204)==0);
    memset(b,0xa5,sizeof b);
    assert(!auto_encode_status_ring(b,335,1,0,counters,ring,reject,ranges) && b[0]==0xa5);
    assert(!auto_encode_status_ring(b,sizeof b,1,0,counters,ring,NULL,ranges) && b[0]==0xa5);
    assert(!auto_encode_status_ring(b,sizeof b,1,0,counters,ring,reject,NULL) && b[0]==0xa5);
    assert(auto_encode_status(b,sizeof b,1,0,counters,ranges)==256);
    for(unsigned i=0;i<16;i++) assert(u32(b+128+8*i)==(i<10 ? i+1:0) && u32(b+132+8*i)==0);
    assert(!auto_encode_status(b,255,1,0,counters,ranges));
    assert(auto_encode_status(b,sizeof b,1,0,counters,NULL)==128);   /* the STATUS of a REFUSED CONFIG */
    auto_queue q;auto_queue_entry *entries=calloc(16384,sizeof *entries);assert(entries);
    assert(!auto_queue_init(&q,entries,16384,252));
    assert(auto_queue_init_wide(&q,entries,16384,252));
    auto_queue_entry *e=auto_queue_claim_snapshot(&q);assert(e && e->pair==UINT32_MAX && e->op.id==1);
    assert(q.free_pairs==252 && auto_queue_held(&q)==0);
    assert(auto_queue_cancel_snapshot(&q));assert(!auto_queue_peek(&q));
    e=auto_queue_claim_snapshot(&q);assert(e && e->op.id==1);auto_queue_commit(&q);
    assert(!auto_queue_ack(&q,1,1,0));assert(auto_queue_ack(&q,1,1,1));
    assert(q.free_pairs==252 && auto_queue_held(&q)==0);
    e=auto_queue_claim(&q);assert(e && e->op.id==2 && q.free_pairs==251);
    assert(!auto_queue_cancel_snapshot(&q));auto_queue_commit(&q);
    e=auto_queue_claim_snapshot(&q);assert(e && e->op.id==3);auto_queue_commit(&q);
    assert(auto_queue_ack(&q,2,1,1));assert(auto_queue_ack(&q,3,0,1));
    assert(q.free_pairs==252 && auto_queue_held(&q)==0);
    printf("ring wire/queue PASS: metadata record bytes=%zu\n",sizeof *entries);
    free(entries);return 0;
}
