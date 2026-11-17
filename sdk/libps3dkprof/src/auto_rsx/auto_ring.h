#ifndef PS3DK_PROF_AUTO_RING_H
#define PS3DK_PROF_AUTO_RING_H
#include <stdint.h>

#define auto_ring_init ps3dk_prof__auto_ring_init
#define auto_ring_admit ps3dk_prof__auto_ring_admit
#define auto_ring_seal ps3dk_prof__auto_ring_seal
#define auto_ring_ready ps3dk_prof__auto_ring_ready
#define auto_ring_ack ps3dk_prof__auto_ring_ack
#define auto_ring_stop ps3dk_prof__auto_ring_stop
#define auto_ring_halt ps3dk_prof__auto_ring_halt
#define auto_ring_quarantine ps3dk_prof__auto_ring_quarantine

enum { AUTO_RING_OK, AUTO_RING_PENDING, AUTO_RING_FULL, AUTO_RING_METADATA,
       AUTO_RING_BOUNDARY, AUTO_RING_SERIAL, AUTO_RING_INVALID };
enum { AUTO_RING_GENERATION_MAX=UINT32_MAX/2u-1u, AUTO_RING_PAIRS_MAX=126 };

typedef struct auto_ring_slot {
    uint64_t frame, window;
    uint32_t generation, count;
    uint32_t closed; /* producer release: 0 filling, 1 emitted, 2 uncertain */
    uint32_t acked, ready; /* consumer only, until retirement */
    uint32_t state; /* producer, before closed: the flush's M2MF case, 0 untouched, 1 restored */
} auto_ring_slot;

typedef struct auto_ring_ticket {
    uint32_t generation, local, slot, pair;
} auto_ring_ticket;

typedef struct auto_ring {
    auto_ring_slot *slots;
    uint32_t capacity, blocks, pairs;
    uint32_t issued; /* producer release; labels may not exceed twice this */
    uint32_t retired; /* consumer release; frees a MAIN extent, NOT local reports */
    uint32_t invalid; /* sticky, either side can invalidate */
    uint32_t halted; /* sticky, producer only: no new admission; issued blocks still drain and retire */
    uint32_t active; /* producer only */
    uint32_t highest_label; /* consumer only, every observation is checked */
} auto_ring;

/* All descriptors and DMA memory are allocated before init. No allocation or
 * device reads here. Exactly one producer and one consumer. The command emitter
 * must order copy-completion on the GPU before reusing a local generation; this
 * component deliberately does not substitute PPU publication for that edge. */
int auto_ring_init(auto_ring *, auto_ring_slot *, uint32_t slots, uint32_t blocks, uint32_t pairs);
/* metadata_reserved is a real reservation, made before this call. A new block
 * also requires the caller's physical flush-command credit before admission.
 * Refusal changes no destination, generation, or draw count. */
int auto_ring_admit(auto_ring *, uint64_t frame, uint64_t window, int metadata_reserved, auto_ring_ticket *);
/* After complete flush emission, or on uncertain emission. No rollback of
 * admitted work. A false emitted argument quarantines the ring. */
int auto_ring_seal(auto_ring *, int emitted);
/* Both sides: label anomalies and uncertain emission. Nothing is read or retired afterwards. */
void auto_ring_stop(auto_ring *);
/* Producer only: admission ends for good, nothing else changes. Blocks issued with complete flushes still become
 * ready and retire; the consumer refuses a quarantined block (never reads it) without stopping the ring. */
void auto_ring_halt(auto_ring *);
/* Producer: the open block's flush was refused before any of it was stored in the command stream. The block closes
 * uncertain (never read, never reused) and admission halts; blocks sealed earlier still drain. 0 if none open. */
int auto_ring_quarantine(auto_ring *);
/* Consumer: put_frontier is a release/acquire-confirmed published even label
 * target. An observed label above that but within issued work defers; it can be
 * the legal PUT/frontier-store race. Future and regressed labels invalidate. */
int auto_ring_ready(auto_ring *, uint32_t generation, uint32_t label, uint32_t put_frontier);
/* Consumer: one exact operation in the oldest extent, after ready and whole
 * sink acceptance. Last ack releases the extent. Short writes release nothing. */
int auto_ring_ack(auto_ring *, uint32_t generation, uint32_t ordinal, int whole);
#endif
