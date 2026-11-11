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

/* Sixteen physical clock reports, reused; a separate bounded record ring. */
enum { AUTO_CLOCK_SLOTS=16, AUTO_CLOCK_RECORDS=16, AUTO_CLOCK_POLLS=100000 };
/* Physical slot states. Only the owner moves FREE -> HELD (when it issues an
 * anchor); only the drainer moves HELD -> FREE or HELD -> QUARANTINED, and only
 * on whole sink acceptance of that slot's record. QUARANTINED is terminal for
 * the instance. */
enum { AUTO_CLOCK_FREE=0, AUTO_CLOCK_HELD=1, AUTO_CLOCK_QUARANTINED=2 };
typedef struct auto_clock {
    auto_wire_clock records[AUTO_CLOCK_RECORDS];
    uint32_t release[AUTO_CLOCK_RECORDS]; /* written with its record, before publication */
    uint32_t slots[AUTO_CLOCK_SLOTS];
    uint32_t instance,epoch,first;
    uint64_t frequency,attempted,lost;
    uint32_t saturated,prepared,published;
    uint64_t write_idx,read_idx; /* nonwrapping: owner release / drainer release */
    uint64_t quarantined;        /* drainer: slots retired for the rest of the instance */
    /* owner only; no nested sample */
    uint32_t active,last_label,invalid,serial,cursor;
} auto_clock;
typedef struct auto_clock_port {
    void *user;
    uint64_t (*tb)(void *); /* ordered around device accesses */
    int (*live)(void *); /* immutable lease/epoch still valid, STOP absent */
    /* bounded room, no callback. 0 means no command word was written. */
    int (*emit)(void *,uint32_t report,uint32_t serial);
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
/* Owner only, at scheduled boundaries. 1 publishes an immutable success or
 * failure fact; 2 records capacity loss (no FREE slot, no record room or the
 * serial space is used up) with no device write; 0 rejects reentry/args.
 * Serials are per instance, nonwrapping and independent of the slot index. */
int auto_clock_sample(auto_clock *,const auto_clock_port *,uint32_t owner,uint64_t frame);
uint32_t auto_clock_before(const auto_clock *); /* before the original PUT */
int auto_clock_after(auto_clock *,uint32_t); /* after original PUT + barrier */
const auto_wire_clock *auto_clock_peek(auto_clock *);
/* Drainer only. whole=1 after the sink accepted the complete record. A valid
 * completion, or an attempt that wrote no command, frees its slot; any other
 * outcome quarantines it. A partial write releases nothing. */
int auto_clock_ack(auto_clock *,uint32_t id,int whole);
#endif
