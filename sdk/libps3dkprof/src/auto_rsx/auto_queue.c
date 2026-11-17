#include "auto_queue.h"
#include <string.h>

#define ACQ(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define RLX(p) __atomic_load_n((p),__ATOMIC_RELAXED)
#define REL(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
_Static_assert(__atomic_always_lock_free(4,0), "AUTO queue needs inline 4-byte atomics");
_Static_assert(__atomic_always_lock_free(8,0), "AUTO queue needs inline 8-byte atomics");

int auto_queue_init(auto_queue *q, auto_queue_entry *entries, uint32_t capacity, uint32_t pairs)
{
    if(!q || !entries || !capacity || capacity>AUTO_METADATA_MAX || !pairs || pairs>AUTO_PAIRS_MAX) return 0;
    memset(q,0,sizeof *q);
    memset(entries,0,(size_t)capacity*sizeof *entries);
    q->entries=entries; q->capacity=capacity; q->pairs=pairs;
    q->next_id=1; q->next_serial=1; q->free_pairs=pairs;
    return 1;
}

/* The free pairs class 0 must leave: each reserve's unheld part. */
static uint32_t keep_free(const auto_queue *q)
{
    uint32_t c,keep=0;
    for(c=1;c<AUTO_PAIR_CLASSES;c++) {
        uint32_t h=ACQ(&q->held[c]);
        if(h<q->cap[c]) keep+=q->cap[c]-h;
    }
    return keep;
}
void auto_queue_set_reserve(auto_queue *q, uint32_t cls, uint32_t cap)
{
    if(cls>0 && cls<AUTO_PAIR_CLASSES) q->cap[cls]=cap;
}
auto_queue_entry *auto_queue_claim(auto_queue *q)
{
    uint64_t w,r;
    uint32_t i,pair=0;
    auto_queue_entry *e;
    q->refusal=AUTO_QUEUE_NONE;
    if(ACQ(&q->stopped)) {q->refusal=AUTO_QUEUE_STOPPED;return NULL;}
    if(q->claimed) {q->refusal=AUTO_QUEUE_REENTRY;return NULL;}
    if(q->next_serial>UINT32_MAX-2u || q->next_id==UINT64_MAX) {q->refusal=AUTO_QUEUE_EXHAUSTED;return NULL;}
    w=RLX(&q->write_idx); r=ACQ(&q->read_idx);
    if(w-r>=q->capacity) {q->refusal=AUTO_QUEUE_METADATA;return NULL;}
    if(ACQ(&q->free_pairs)<=keep_free(q)) {q->refusal=AUTO_QUEUE_REPORTS;return NULL;}
    for(i=0;i<q->pairs;i++) {
        pair=(q->pair_cursor+i)%q->pairs;
        if(ACQ(&q->pair_state[pair])==0) break;
    }
    if(i==q->pairs) {q->refusal=AUTO_QUEUE_REPORTS;return NULL;}
    q->pair_class[pair]=AUTO_PAIR_DRAW;
    REL(&q->pair_state[pair],1u);
    __atomic_fetch_sub(&q->free_pairs,1u,__ATOMIC_ACQ_REL);
    q->pair_cursor=(pair+1u)%q->pairs;
    e=&q->entries[w%q->capacity];
    memset(e,0,sizeof *e);
    e->pair=pair; e->op.id=q->next_id++;
    e->op.serial_begin=q->next_serial; e->op.serial_end=q->next_serial+1u; q->next_serial+=2u;
    q->claimed=1;
    return e;
}

static int take_pair(auto_queue *q,uint32_t cls)
{
    uint32_t i,pair=0;
    q->refusal=AUTO_QUEUE_NONE;
    if(ACQ(&q->stopped)) {q->refusal=AUTO_QUEUE_STOPPED;return -1;}
    if(cls ? (ACQ(&q->held[cls])>=q->cap[cls] || !ACQ(&q->free_pairs)) : ACQ(&q->free_pairs)<=keep_free(q)) {
        q->refusal=AUTO_QUEUE_REPORTS;return -1;
    }
    for(i=0;i<q->pairs;i++) {
        pair=(q->pair_cursor+i)%q->pairs;
        if(ACQ(&q->pair_state[pair])==0) break;
    }
    if(i==q->pairs) {q->refusal=AUTO_QUEUE_REPORTS;return -1;}
    q->pair_class[pair]=(uint8_t)cls;
    if(cls) __atomic_fetch_add(&q->held[cls],1u,__ATOMIC_ACQ_REL);
    REL(&q->pair_state[pair],1u);
    __atomic_fetch_sub(&q->free_pairs,1u,__ATOMIC_ACQ_REL);
    q->pair_cursor=(pair+1u)%q->pairs;
    return (int)pair;
}
int auto_queue_take_pair(auto_queue *q) {return take_pair(q,AUTO_PAIR_DRAW);}
int auto_queue_take_class_pair(auto_queue *q, uint32_t cls)
{
    if(cls<1 || cls>=AUTO_PAIR_CLASSES) {q->refusal=AUTO_QUEUE_REPORTS;return -1;}
    return take_pair(q,cls);
}
int auto_queue_take_reserved_pair(auto_queue *q) {return auto_queue_take_class_pair(q,AUTO_PAIR_CALL);}

void auto_queue_quarantine_pair(auto_queue *q, uint32_t pair)
{
    if(pair<q->pairs) REL(&q->pair_state[pair],2u);
}

auto_queue_entry *auto_queue_claim_held(auto_queue *q, uint32_t pair, uint32_t share)
{
    uint64_t w,r;
    auto_queue_entry *e;
    q->refusal=AUTO_QUEUE_NONE;
    if(pair>=q->pairs || ACQ(&q->pair_state[pair])!=1u) {q->refusal=AUTO_QUEUE_REPORTS;return NULL;}
    if(ACQ(&q->stopped)) {q->refusal=AUTO_QUEUE_STOPPED;return NULL;}
    if(q->claimed) {q->refusal=AUTO_QUEUE_REENTRY;return NULL;}
    if(q->next_id==UINT64_MAX || (!share && q->next_serial>UINT32_MAX-2u) ||
       (share && (share&1u || share+1u!=q->next_serial))) {q->refusal=AUTO_QUEUE_EXHAUSTED;return NULL;}
    w=RLX(&q->write_idx); r=ACQ(&q->read_idx);
    if(w-r>=q->capacity) {q->refusal=AUTO_QUEUE_METADATA;return NULL;}
    e=&q->entries[w%q->capacity];
    memset(e,0,sizeof *e);
    e->pair=pair; e->op.id=q->next_id++;
    if(share) {e->op.serial_begin=share-1u; e->op.serial_end=share;}
    else {e->op.serial_begin=q->next_serial; e->op.serial_end=q->next_serial+1u; q->next_serial+=2u;}
    q->claimed=1;
    return e;
}

void auto_queue_commit(auto_queue *q)
{
    if(!q->claimed) return;
    q->claimed=0;
    REL(&q->write_idx,RLX(&q->write_idx)+1u);
}

const auto_queue_entry *auto_queue_peek(auto_queue *q)
{
    uint64_t r=RLX(&q->read_idx);
    if(r==ACQ(&q->write_idx)) return NULL;
    return &q->entries[r%q->capacity];
}

const auto_queue_entry *auto_queue_peek_at(auto_queue *q, uint64_t k)
{
    uint64_t r=RLX(&q->read_idx), w=ACQ(&q->write_idx);
    if(w-r<=k) return NULL;
    return &q->entries[(r+k)%q->capacity];
}

int auto_queue_ack(auto_queue *q, uint64_t id, int completed, int whole)
{
    const auto_queue_entry *e=auto_queue_peek(q);
    uint64_t r=RLX(&q->read_idx);
    if(!whole || !e || e->op.id!=id) return 0;
    if(completed && !ACQ(&q->stopped)) {
        uint32_t cls=q->pair_class[e->pair];
        if(cls) __atomic_fetch_sub(&q->held[cls],1u,__ATOMIC_ACQ_REL);
        REL(&q->pair_state[e->pair],0u);
        __atomic_fetch_add(&q->free_pairs,1u,__ATOMIC_ACQ_REL);
    } else {
        REL(&q->pair_state[e->pair],2u);
    }
    q->written++;
    REL(&q->read_idx,r+1u); /* no slot can change until sink acceptance and every read are finished */
    return 1;
}

void auto_queue_stop(auto_queue *q) { REL(&q->stopped,1u); }
uint32_t auto_queue_held(const auto_queue *q)
{
    uint32_t i,n=0;
    for(i=0;i<q->pairs;i++) n+=ACQ(&q->pair_state[i])!=0;
    return n;
}
