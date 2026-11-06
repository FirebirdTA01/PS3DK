#ifndef HETEROGENEOUS_WORKLOAD_H
#define HETEROGENEOUS_WORKLOAD_H
#include <stdint.h>
#define WORK_ITEMS 16384u
#define WORK_ROUNDS 128u
#define WORK_DIGEST 0xe7b15af8u
#define WORK_MAX_SPUS 6u
typedef struct { uint32_t digest, count; } work_result;
static inline uint32_t work_mix(uint32_t index, uint32_t rounds)
{
    /* This is a workload, not a precomputable lookup table. A volatile local
     * read makes each invocation observable and prevents loop-invariant
     * reuse across frames while keeping the arithmetic/output identical. */
    volatile uint32_t seed=index^0x9e3779b9u;
    uint32_t x=seed;
    for(uint32_t i=0;i<rounds;++i) {
        x^=x>>13;
        x=x*1664525u+1013904223u+i;
        x=(x<<7)|(x>>25);
    }
    return x;
}
static inline work_result work_partition(uint32_t lane, uint32_t lanes, uint32_t items)
{
    work_result r={0,0};
    if(!lanes || lanes>WORK_MAX_SPUS || lane>=lanes || items>WORK_ITEMS)return r;
    for(uint32_t i=lane;i<items;i+=lanes){r.digest^=work_mix(i,WORK_ROUNDS);++r.count;}
    return r;
}
static inline int work_phase(uint64_t elapsed, uint64_t duration)
{ return duration ? (int)((elapsed/duration)%4) : -1; }
#endif
