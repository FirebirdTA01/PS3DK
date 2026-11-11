#ifndef PS3DK_PROF_AUTO_QUEUE_H
#define PS3DK_PROF_AUTO_QUEUE_H
#include "auto_wire.h"

#define auto_queue_init ps3dk_prof__auto_queue_init
#define auto_queue_claim ps3dk_prof__auto_queue_claim
#define auto_queue_commit ps3dk_prof__auto_queue_commit
#define auto_queue_peek ps3dk_prof__auto_queue_peek
#define auto_queue_ack ps3dk_prof__auto_queue_ack
#define auto_queue_stop ps3dk_prof__auto_queue_stop
#define auto_queue_held ps3dk_prof__auto_queue_held
#define auto_queue_take_pair ps3dk_prof__auto_queue_take_pair
#define auto_queue_quarantine_pair ps3dk_prof__auto_queue_quarantine_pair
#define auto_queue_claim_held ps3dk_prof__auto_queue_claim_held

enum { AUTO_PAIRS_MAX=504, AUTO_METADATA_MAX=1024 };
enum { AUTO_QUEUE_NONE, AUTO_QUEUE_STOPPED, AUTO_QUEUE_REENTRY,
       AUTO_QUEUE_EXHAUSTED, AUTO_QUEUE_METADATA, AUTO_QUEUE_REPORTS };
typedef struct auto_queue_entry {
    auto_wire_op op;
    uint32_t pair;
} auto_queue_entry;

typedef struct auto_queue {
    auto_queue_entry *entries; /* allocated once, immutable while visible */
    uint32_t capacity, pairs;
    uint64_t write_idx, read_idx; /* owner release / consumer release respectively */
    uint32_t stopped;
    uint32_t pair_state[AUTO_PAIRS_MAX]; /* 0 free, 1 owned, 2 quarantined */
    /* owner only */
    uint32_t claimed, pair_cursor, next_serial, refusal;
    uint64_t next_id;
    /* consumer only */
    uint64_t written;
} auto_queue;

/* One producer and one consumer. init requires externally quiescent storage.
 * claim/commit enclose the complete emission; even an uncertain emission must
 * commit an INCOMPLETE record. stop prevents later admission but does not undo
 * an in-flight claim. Capture seal must acquire the owner's acknowledgement or
 * retain the allocation forever; queue emptiness is not owner quiescence. */
int auto_queue_init(auto_queue *, auto_queue_entry *, uint32_t capacity, uint32_t pairs);
auto_queue_entry *auto_queue_claim(auto_queue *);
void auto_queue_commit(auto_queue *);
const auto_queue_entry *auto_queue_peek(auto_queue *);
/* completed is supplied by validated device/report facts. This queue does not
 * read or infer them. Whole sink acceptance must belong to the exact head id. */
int auto_queue_ack(auto_queue *, uint64_t id, int completed, int whole);
void auto_queue_stop(auto_queue *);
uint32_t auto_queue_held(const auto_queue *); /* live scan, not an exact concurrent snapshot */
/* Owner only, for CALL segments: a report pair owned before its record exists
 * (reports written inside a segment that is executed later). Returns the pair
 * index or -1 with refusal set. */
int auto_queue_take_pair(auto_queue *);
/* A taken pair whose reports may still be written but whose completion can
 * never be established (segment rebuilt, never called, called elsewhere). */
void auto_queue_quarantine_pair(auto_queue *, uint32_t pair);
/* claim with a taken pair. share = 0 takes a new serial pair; otherwise share
 * is the serial_end of the previous record of the same CALL, which completes
 * every record of that call at once. */
auto_queue_entry *auto_queue_claim_held(auto_queue *, uint32_t pair, uint32_t share);
#endif
