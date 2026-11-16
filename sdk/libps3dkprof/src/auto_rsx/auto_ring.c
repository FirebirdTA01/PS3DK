#include "auto_ring.h"
#include <stddef.h>
#include <string.h>

#define ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define REL(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
_Static_assert(__atomic_always_lock_free(4,0), "report ring requires lock-free serials");

int auto_ring_init(auto_ring *r, auto_ring_slot *slots, uint32_t capacity, uint32_t blocks, uint32_t pairs)
{
    if(!r || !slots || !capacity || capacity>524288u || blocks<2 || blocks>4 ||
       !pairs || pairs>AUTO_RING_PAIRS_MAX) return 0;
    memset(r,0,sizeof *r);
    memset(slots,0,(size_t)capacity*sizeof *slots);
    r->slots=slots;r->capacity=capacity;r->blocks=blocks;r->pairs=pairs;
    return 1;
}

void auto_ring_stop(auto_ring *r) { REL(&r->invalid,1u); }

int auto_ring_admit(auto_ring *r, uint64_t frame, uint64_t window, int metadata, auto_ring_ticket *out)
{
    auto_ring_slot *s;
    uint32_t seq=r->active;
    if(ACQ(&r->invalid)) return AUTO_RING_INVALID;
    if(!out || !metadata) return AUTO_RING_METADATA;
    if(seq) {
        s=&r->slots[(seq-1u)%r->capacity];
        if(s->frame!=frame || s->window!=window || s->count==r->pairs) return AUTO_RING_BOUNDARY;
    } else {
        uint32_t issued=ACQ(&r->issued);
        if(issued==AUTO_RING_GENERATION_MAX) return AUTO_RING_SERIAL;
        if(issued-ACQ(&r->retired)>=r->capacity) return AUTO_RING_FULL;
        seq=issued+1u;
        s=&r->slots[(seq-1u)%r->capacity];
        /* Acquiring retired proves the consumer no longer references this
         * descriptor or its DMA extent. Reset before publishing the new id. */
        memset(s,0,sizeof *s);
        s->generation=seq;s->frame=frame;s->window=window;
        r->active=seq;
        REL(&r->issued,seq);
    }
    *out=(auto_ring_ticket){seq,(seq-1u)%r->blocks,(seq-1u)%r->capacity,s->count++};
    return AUTO_RING_OK;
}

int auto_ring_seal(auto_ring *r, int emitted)
{
    uint32_t seq=r->active;
    if(!seq) return 0;
    auto_ring_slot *s=&r->slots[(seq-1u)%r->capacity];
    if(!emitted) auto_ring_stop(r);
    REL(&s->closed,emitted ? 1u:2u);
    r->active=0;
    return 1;
}

int auto_ring_ready(auto_ring *r, uint32_t seq, uint32_t label, uint32_t frontier)
{
    uint32_t issued=ACQ(&r->issued),retired=ACQ(&r->retired);
    if(ACQ(&r->invalid)) return AUTO_RING_INVALID;
    if(label<r->highest_label || label>2u*issued || frontier>2u*issued || (frontier&1u)) {
        auto_ring_stop(r);return AUTO_RING_INVALID;
    }
    r->highest_label=label;
    if(!seq || seq<=retired || seq>issued) return AUTO_RING_INVALID;
    auto_ring_slot *s=&r->slots[(seq-1u)%r->capacity];
    uint32_t closed=ACQ(&s->closed);
    if(!closed) return AUTO_RING_PENDING;
    if(closed!=1 || s->generation!=seq) {auto_ring_stop(r);return AUTO_RING_INVALID;}
    if(label>frontier || label<2u*seq) return AUTO_RING_PENDING;
    s->ready=1;
    return AUTO_RING_OK;
}

int auto_ring_ack(auto_ring *r, uint32_t seq, uint32_t ordinal, int whole)
{
    if(!whole || ACQ(&r->invalid) || seq!=ACQ(&r->retired)+1u || seq>ACQ(&r->issued)) return 0;
    auto_ring_slot *s=&r->slots[(seq-1u)%r->capacity];
    if(ACQ(&s->closed)!=1 || s->generation!=seq || !s->ready || ordinal!=s->acked || ordinal>=s->count) return 0;
    if(++s->acked==s->count) REL(&r->retired,seq);
    return 1;
}
