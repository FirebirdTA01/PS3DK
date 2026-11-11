#include "auto_frames.h"
#include <string.h>
#define ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define REL(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
_Static_assert(__atomic_always_lock_free(8,0),"AUTO frames need inline 8-byte atomics");

int auto_frames_init(auto_frames *q,auto_wire_frame *entries,uint32_t capacity,uint32_t instance,uint32_t owner)
{
    if(!q || !entries || !capacity || capacity>256 || !instance || !owner) return 0;
    memset(q,0,sizeof *q);memset(entries,0,(size_t)capacity*sizeof *entries);
    q->entries=entries;q->capacity=capacity;q->instance=instance;q->owner=owner;
    return 1;
}

int auto_frames_count(auto_frames *q,uint32_t kind,uint32_t column)
{
    uint64_t *value;
    if(!q->active || kind<1 || kind>3 || column>=12) return 0;
    value=&q->live.counts[kind-1][column];
    if(*value==UINT64_MAX) {q->live.flags|=4;REL(&q->saturated,1u);}
    else ++*value;
    return 1;
}

int auto_frames_close(auto_frames *q,uint32_t owner,uint64_t frame,uint64_t tb,int open)
{
    uint64_t w,r;
    if(!q->active || owner!=q->owner || frame!=q->live.frame) return 0;
    if(!(q->live.flags&4)) for(unsigned kind=0;kind<3;kind++) {
        uint64_t remaining=q->live.counts[kind][0];
        for(unsigned column=1;column<12;column++) {
            if(q->live.counts[kind][column]>remaining) return 0;
            remaining-=q->live.counts[kind][column];
        }
        if(remaining) return 0;
    }
    q->live.tb_end=tb;q->live.flags|=2;
    if(open || tb<q->live.tb_begin) q->live.flags|=16;
    w=ACQ(&q->write_idx);r=ACQ(&q->read_idx);
    q->active=0;
    if(w==UINT64_MAX || w-r>=q->capacity) {
        uint64_t lost=ACQ(&q->lost);
        if(lost==UINT64_MAX) REL(&q->saturated,1u);
        else REL(&q->lost,lost+1u);
        return 2;
    }
    q->live.lost=ACQ(&q->lost);
    if(q->live.lost) q->live.flags|=8;
    q->entries[w%q->capacity]=q->live;
    REL(&q->write_idx,w+1u);
    return 1;
}

int auto_frames_open(auto_frames *q,uint64_t frame,uint64_t tb)
{
    if(frame==UINT64_MAX || (q->seen && frame<=q->last)) return 0;
    if(q->active && !auto_frames_close(q,q->owner,q->live.frame,tb,1)) return 0;
    memset(&q->live,0,sizeof q->live);
    q->live.flags=1;q->live.instance=q->instance;q->live.owner=q->owner;
    q->live.frame=frame;q->live.tb_begin=tb;
    q->active=q->seen=1;q->last=frame;
    return 1;
}

const auto_wire_frame *auto_frames_peek(auto_frames *q)
{
    uint64_t r=ACQ(&q->read_idx);
    return r==ACQ(&q->write_idx) ? NULL:&q->entries[r%q->capacity];
}
int auto_frames_ack(auto_frames *q,uint32_t owner,uint64_t frame,int whole)
{
    const auto_wire_frame *f=auto_frames_peek(q);
    if(!whole || !f || f->owner!=owner || f->frame!=frame) return 0;
    REL(&q->read_idx,ACQ(&q->read_idx)+1u);
    return 1;
}
