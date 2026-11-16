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
    assert(auto_encode_config(b,sizeof b,&c)==176 && u32(b+24)==1023 && u32(b+60)==160);
    c.ring_bytes=16777216;c.ring_blocks=2;c.ring_pairs=126;c.ring_first=1544;c.ring_label=245;
    c.ring_stride=4096;c.ring_slots=4096;c.metadata_stride=sizeof(auto_queue_entry);c.legacy_pairs=252;
    assert(auto_encode_config(b,sizeof b,&c)==216 && u32(b+24)==2047 && u32(b+60)==176);
    assert(u32(b+176)==16777216 && u32(b+180)==2 && u32(b+184)==126 && u32(b+208)==252);
    memset(b,0xa5,sizeof b);assert(!auto_encode_config(b,215,&c) && b[0]==0xa5);
    op.flags=AUTO_OP_RING;op.ring_generation=3;op.ring_slot=2;op.ring_ordinal=9;op.ring_local=0;
    assert(auto_encode_ops_ring(b,sizeof b,1,&op,1)==192 && u32(b+12)==176);
    assert(u32(b+16+160)==3 && u32(b+16+164)==2 && u32(b+16+168)==9);
    assert(auto_encode_ops(b,sizeof b,1,&op,1)==176 && u32(b+12)==160);
    f.ring_full[0]=3;f.ring_state[2]=4;
    assert(auto_encode_frame_ring(b,sizeof b,&f)==528 && u32(b+480)==3 && u32(b+520)==4);
    assert(auto_encode_frame(b,sizeof b,&f)==480);
    uint64_t counters[15]={0},ring[8]={7,3,14,4,224,4,3,99};
    assert(auto_encode_status_ring(b,sizeof b,1,0,counters,ring)==192);
    assert(u32(b+128)==7 && u32(b+152)==4 && u32(b+184)==99);
    memset(b,0xa5,sizeof b);
    assert(!auto_encode_status_ring(b,191,1,0,counters,ring) && b[0]==0xa5);
    assert(auto_encode_status(b,sizeof b,1,0,counters)==128);
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
