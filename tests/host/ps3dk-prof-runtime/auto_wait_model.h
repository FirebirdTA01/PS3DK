#ifndef AUTO_WAIT_HOST_MODEL_H
#define AUTO_WAIT_HOST_MODEL_H
#include <stdint.h>
/* Host grammar only: durations are timebase deltas, never GPU busy time. */
enum awm_domain { AWM_CPU_POLL=1,AWM_CPU_CALL_OPAQUE,AWM_GPU_COMMAND,AWM_INTERNAL };
enum awm_outcome { AWM_OPEN=0,AWM_SUCCESS,AWM_ERROR,AWM_DEADLINE,AWM_CLOCK,AWM_STOPPED };
#define AWM_RECORDS 32u
#define AWM_DEPTH 8u
typedef struct awm_record {
    uint64_t id,parent,frame,begin,end,target,value,duration;
    uint32_t actor,context,reason,domain,owner,outcome,valid;
} awm_record;
typedef struct awm_model {
    awm_record records[AWM_RECORDS];
    uint32_t used,depth,stack[AWM_DEPTH],stopped,loss;
    uint64_t next_id,refused,orphan,internal,commands,last_tb;
    uint32_t have_tb;
} awm_model;
uint64_t awm_begin(awm_model *m,uint32_t actor,uint32_t context,uint32_t reason,uint32_t domain,uint64_t parent,uint32_t owner,uint64_t frame,uint64_t tb,uint64_t target,uint64_t value);
int awm_end(awm_model *m,uint32_t actor,uint64_t id,uint64_t tb,uint32_t outcome);
void awm_stop(awm_model *m);
/* Union of outer observed CPU intervals; UNKNOWN on missing/invalid history. */
int awm_cpu_total(const awm_model *m,uint64_t *ticks);
#endif
