#ifndef AUTO_RSX_HOST_MODEL_H
#define AUTO_RSX_HOST_MODEL_H
/* Host specification model only. Not linked into an SDK archive. */
#include <stddef.h>
#include <stdint.h>

enum arm_kind { ARM_ARRAY=1, ARM_INDEX=2, ARM_CLEAR=3, ARM_UNSAFE=4, ARM_IMMEDIATE=5, ARM_CALL=6 };
enum arm_reason { ARM_OBSERVED=0, ARM_UNSUPPORTED, ARM_CONTEXT, ARM_REENTRY,
    ARM_SIZE, ARM_REPORTS, ARM_METADATA, ARM_ROOM, ARM_EMISSION, ARM_REASONS };
typedef struct arm_operation {
    uint32_t kind, count, offset, start, index_type, mode, mask;
    uint32_t context, generation, frame_owner;
    uint64_t frame;
} arm_operation;

/* Zero means outside the modeled population. No legacy input is repaired. */
uint64_t arm_words(const arm_operation *op);
int arm_words_to_bytes(uint64_t words, uint64_t *bytes);

#define ARM_MAX_PAIRS 512u
#define ARM_MAX_FRAMES 129u
enum arm_state { ARM_FREE, ARM_CLAIMED, ARM_EMITTED, ARM_PUBLISHED, ARM_COMPLETE, ARM_RETIRED, ARM_QUARANTINE };
typedef struct arm_ticket {
    uint64_t id, raw[2],frame;
    uint32_t state, generation, serial[2], complete, metadata_accepted,frame_owner,kind;
} arm_ticket;
typedef struct arm_frame {
    uint64_t id, visited[3], observed[3], excluded[3][ARM_REASONS], unsupported;
    uint32_t owner, closed;
} arm_frame;
typedef struct arm_model {
    uint32_t *ring, capacity, current, context, generation, pair_limit, metadata_limit;
    uint32_t busy, stopped, report_conflict, next_serial, frames_used, coverage_lost,report_base,label;
    uint64_t calls, app_words, app_hash, marker_words, wraps, phases[2], next_ticket;
    uint32_t put, get;
    uint64_t clock;
    arm_ticket tickets[ARM_MAX_PAIRS];
    arm_frame frames[ARM_MAX_FRAMES];
} arm_model;
typedef int (*arm_emitter)(arm_model *,const arm_operation *,void *);
typedef struct arm_result { uint32_t reason; int application_result; uint64_t ticket; } arm_result;
void arm_init(arm_model *m,uint32_t *ring,uint32_t capacity,uint32_t pairs,uint32_t metadata);
int arm_frame_open(arm_model *m,uint32_t owner,uint64_t frame);
int arm_frame_close(arm_model *m,uint32_t owner,uint64_t frame);
int arm_emit_packet(arm_model *m,const uint32_t *words,uint32_t n);
arm_result arm_submit(arm_model *m,const arm_operation *op,arm_emitter emit,void *user);
int arm_publish(arm_model *m,uint32_t context,uint32_t generation,uint64_t frontier);
int arm_gpu_complete(arm_model *m,uint64_t ticket,uint32_t generation,unsigned half,uint32_t serial,uint64_t raw);
int arm_sink_result(arm_model *m,uint64_t ticket,int accepted_whole);
int arm_drain_offer(arm_model *m,uint64_t ticket);
void arm_stop(arm_model *m);
#define ARM_REGISTRY_SIZE 8u
typedef struct arm_claim { uint32_t owner,generation,first,count,label; } arm_claim;
typedef struct arm_registry { arm_claim claims[ARM_REGISTRY_SIZE]; } arm_registry;
int arm_claim_reports(arm_registry *r,uint32_t owner,uint32_t generation,uint32_t first,uint32_t count,uint32_t label);
int arm_release_reports(arm_registry *r,uint32_t owner,uint32_t generation,const arm_model *m);
int arm_bind_reports(arm_model *m,arm_registry *r,uint32_t first,uint32_t count,uint32_t label);
#endif
