/* Private accepted-output coordinator, not wired into the SDK runtime.
 * A trusted family encoder is required; opaque checkpoints alone are not
 * semantic validation. Single consumer, no allocation or internal locks. */
#ifndef PS3DK_PROF_ROLL_OUTPUT_H
#define PS3DK_PROF_ROLL_OUTPUT_H
#include "roll_store.h"
#ifdef __cplusplus
extern "C" {
#endif
#define pfo_state_bytes ps3dk_prof__roll_output_state_bytes
#define pfo_bind ps3dk_prof__roll_output_bind
#define pfo_accept ps3dk_prof__roll_output_accept
#define pfo_flush ps3dk_prof__roll_output_flush
#define pfo_finish ps3dk_prof__roll_output_finish
#define pfo_get_view ps3dk_prof__roll_output_get_view
#define pfo_support ps3dk_prof__roll_output_support

typedef struct pfo_output pfo_output;
enum { PFO_RESOURCE = -7 };
enum { PFO_READY = 1, PFO_STOPPING = 2, PFO_FINISHED = 3, PFO_POISONED = 4 };
typedef struct {
    size_t checkpoint_bytes;
    uint32_t terminal; /* exactly one iff the actual chunk is TRAILER */
    uint32_t reason;   /* PFR resource reason iff prepare returns PFO_RESOURCE */
} pfo_prepared;
/* The encoder owns semantic validation. It must leave accepted immutable,
 * populate the entire prepared arena, and encode its canonical successor.
 * Arenas must use offsets, not pointers into their own relocatable storage.
 * No IO, producer acknowledgements, reentrancy or external mutation here.
 * Return OK, RESOURCE with reason, or INVALID; failures expose no new state.
 * A genuine accepted TRAILER returns terminal=1, checkpoint_bytes=0. */
typedef int (*pfo_prepare)(void *ctx, const void *accepted, void *prepared,
    const void *chunk, size_t chunk_bytes, void *checkpoint, size_t capacity,
    pfo_prepared *result);
/* Optional bounded-support policy, installed before the first accept. Pressure
 * examines a validated candidate. Projection runs only after a successful
 * segment commit, on a copy of accepted state, with the store's actual oldest
 * retained chunk. It must preserve original frontiers and open source state.
 * No allocation, IO or ACKs. A zero/overlarge checkpoint is an error. */
typedef int (*pfo_pressure)(void *ctx,const void *candidate);
typedef int (*pfo_project)(void *ctx,const void *accepted,void *projected,
    uint64_t first_retained,void *checkpoint,size_t capacity,size_t *written);
typedef struct {
    void *arenas[2];
    size_t arena_bytes;
    void *checkpoints[3];
    size_t checkpoint_capacity;
    void *payload;
    size_t payload_capacity;
    void *encoder_ctx;
    pfo_prepare prepare;
} pfo_buffers;
typedef struct {
    uint64_t accepted_next_chunk, accepted_payload_bytes;
    uint64_t committed_next_chunk, committed_payload_bytes;
    size_t pending_bytes;
    uint32_t phase, reason, terminal;
    int failure;
    const void *accepted_arena; /* borrowed, read-only until next accept */
} pfo_view;

size_t pfo_state_bytes(void);
/* Fresh exclusive v2 OPEN store at chunk0; arenas[0] and initial_checkpoint
 * describe the genuine chunk0 state. All buffers, coordinator, store state,
 * store scratch and encoder inputs must be disjoint for their whole lifetime.
 * Store scratch/encoder context ownership remains the caller's contract.
 * No IO on bind. No live reopen/resume or concurrent direct store operations. */
int pfo_bind(pfo_output *, pfr_store *, const pfo_buffers *,
    const void *initial_checkpoint, size_t initial_bytes);
int pfo_support(pfo_output *,pfo_pressure,pfo_project);
/* Whole original framed chunk. OK means owned in preallocated RAM, not durable.
 * Preflight precedes rotation. Every refusal preserves the accepted frontier. */
int pfo_accept(pfo_output *, const void *chunk, size_t chunk_bytes);
int pfo_flush(pfo_output *);
/* Called only by the outer lifecycle owner after producer quiescence.
 * Resource stop flushes accepted bytes, then STOPPED_RESOURCE; successful
 * terminal flushes a genuine TRAILER, then SEALED. Neither proves close/join. */
int pfo_finish(pfo_output *);
int pfo_get_view(const pfo_output *, pfo_view *);
#ifdef __cplusplus
}
#endif
#endif
