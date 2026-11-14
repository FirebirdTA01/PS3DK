#ifndef PS3DK_PROF_AUTO_CLOCK_H
#define PS3DK_PROF_AUTO_CLOCK_H
#include "auto_wire.h"
#define auto_clock_init ps3dk_prof__auto_clock_init
#define auto_clock_room ps3dk_prof__auto_clock_room
#define auto_clock_sample ps3dk_prof__auto_clock_sample
#define auto_clock_before ps3dk_prof__auto_clock_before
#define auto_clock_after ps3dk_prof__auto_clock_after
#define auto_clock_peek ps3dk_prof__auto_clock_peek
#define auto_clock_ack ps3dk_prof__auto_clock_ack
#define auto_clock_poll ps3dk_prof__auto_clock_poll

/* Sixteen physical clock reports, reused; a separate bounded record ring. AUTO_CLOCK_POLLS bounds the drainer polls
 * one anchor may take (the 2 s bound ends it first at any real drain cadence). */
enum { AUTO_CLOCK_SLOTS=16, AUTO_CLOCK_RECORDS=16, AUTO_CLOCK_POLLS=100000 };
/* Record states: the owner moves EMPTY -> PENDING when it issues an anchor (or straight to DONE when nothing could
 * be issued); only the drainer moves PENDING -> DONE; auto_clock_peek sees DONE records only. */
enum { AUTO_CLOCK_EMPTY=0, AUTO_CLOCK_PENDING=1, AUTO_CLOCK_DONE=2 };
/* Physical slot states. Only the owner moves FREE -> HELD (when it issues an
 * anchor); only the drainer moves HELD -> FREE or HELD -> QUARANTINED, and only
 * on whole sink acceptance of that slot's record. QUARANTINED is terminal for
 * the instance. */
enum { AUTO_CLOCK_FREE=0, AUTO_CLOCK_HELD=1, AUTO_CLOCK_QUARANTINED=2 };
typedef struct auto_clock {
    auto_wire_clock records[AUTO_CLOCK_RECORDS];
    uint32_t release[AUTO_CLOCK_RECORDS]; /* written with its record, before publication */
    uint32_t state[AUTO_CLOCK_RECORDS];   /* AUTO_CLOCK_EMPTY/PENDING/DONE, release/acquire */
    uint32_t emitted[AUTO_CLOCK_RECORDS]; /* owner: a command word of this anchor was written */
    uint32_t has_lo[AUTO_CLOCK_RECORDS];  /* drainer: a poll saw the RSX before the anchor */
    uint64_t issue[AUTO_CLOCK_RECORDS];   /* owner: issue time (the 2 s bound counts from it) */
    uint32_t slots[AUTO_CLOCK_SLOTS];
    uint32_t instance,epoch,first;
    uint64_t frequency,attempted,lost;
    uint32_t saturated,prepared,published;
    uint64_t write_idx,read_idx; /* nonwrapping: owner release / drainer release */
    uint64_t quarantined;        /* drainer: slots retired for the rest of the instance */
    uint64_t poll_idx;           /* drainer: oldest record not yet DONE, read_idx <= poll_idx <= write_idx */
    uint32_t last_label,invalid; /* drainer: cumulative label regression check; invalid is read by both */
    /* owner only; no nested sample */
    uint32_t active,serial,cursor;
} auto_clock;
typedef struct auto_clock_port {
    void *user;
    uint64_t (*tb)(void *); /* ordered around device accesses */
    int (*live)(void *); /* immutable lease/epoch still valid, STOP absent */
    /* Bounded room, no callback. Writes, for anchor k, a timestamp into `report` released by label value 2k-1,
     * then a second timestamp into the same report released by 2k: the report's final value is written after the
     * label reaches 2k-1 and before it reaches 2k. Returns 1 when both were written, 0 when no command word was
     * written, 2 when only part was. */
    int (*emit)(void *,uint32_t report,uint32_t k);
    void (*flush)(void *); /* original PUT path calls before/after below */
    uint32_t (*label)(void *);
    uint64_t (*timer)(void *,uint32_t report);
    void (*sync)(void *);
} auto_clock_port;
/* A new instance on a lease nothing has written yet: every slot starts FREE.
 * Slot state never passes between instances or captures. */
int auto_clock_init(auto_clock *,uint32_t instance,uint32_t epoch,uint32_t first,uint64_t frequency);
/* Owner only: 1 when a sample now would find a FREE slot, record room and an
 * unused serial. */
int auto_clock_room(const auto_clock *);
/* Owner only, at scheduled boundaries; never waits for the RSX. 1 issues anchor k (its record becomes PENDING and
 * the drainer completes it) or publishes an immediate failure fact; 2 records capacity loss (no FREE slot, no record
 * room or the serial space is used up) with no device write; 0 rejects reentry/args. Serials are per instance,
 * nonwrapping and independent of the slot index. */
int auto_clock_sample(auto_clock *,const auto_clock_port *,uint32_t owner,uint64_t frame);
uint32_t auto_clock_before(const auto_clock *); /* before the original PUT */
int auto_clock_after(auto_clock *,uint32_t); /* after original PUT + barrier */
const auto_wire_clock *auto_clock_peek(auto_clock *);
/* Drainer only. whole=1 after the sink accepted the complete record. A valid
 * completion, or an attempt that wrote no command, frees its slot; any other
 * outcome quarantines it. A partial write releases nothing. */
int auto_clock_ack(auto_clock *,uint32_t id,int whole);
/* Drainer only, every drain slice and before sealing: one ordered label read completes PENDING anchors in serial
 * order. VALID brackets the timestamp between the last poll that saw the label below 2k-1 (or the issue time) and
 * the first that saw it at or above 2k. seal=1 also ends every anchor still PENDING (TIMEOUT). Returns 0 when the
 * label regressed (the instance is invalid), 1 otherwise. */
int auto_clock_poll(auto_clock *,const auto_clock_port *,int seal);
#endif
