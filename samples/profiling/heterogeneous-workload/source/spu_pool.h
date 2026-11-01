#ifndef HETEROGENEOUS_SPU_POOL_H
#define HETEROGENEOUS_SPU_POOL_H
#include "workload.h"
/* Distinct cache lines: PPU never resets an SPU-owned completion. */
typedef struct __attribute__((aligned(128))) {
    uint32_t digest,count,begin_dec,end_dec;
    uint32_t reserved[28];
    uint32_t sequence;
    uint32_t padding[31];
} work_slot;
#ifndef __SPU__
int pool_init(void);
unsigned pool_count(void);
int pool_submit(uint32_t sequence);
int pool_ready(uint32_t sequence);
const volatile work_slot *pool_slot(unsigned lane);
int pool_close(void);
#endif
#endif
