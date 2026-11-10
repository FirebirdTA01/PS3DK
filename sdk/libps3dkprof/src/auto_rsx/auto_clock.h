#ifndef PS3DK_PROF_AUTO_CLOCK_H
#define PS3DK_PROF_AUTO_CLOCK_H
#include "auto_wire.h"
#define auto_clock_init ps3dk_prof__auto_clock_init
#define auto_clock_sample ps3dk_prof__auto_clock_sample
#define auto_clock_before ps3dk_prof__auto_clock_before
#define auto_clock_after ps3dk_prof__auto_clock_after
#define auto_clock_peek ps3dk_prof__auto_clock_peek
#define auto_clock_ack ps3dk_prof__auto_clock_ack

enum { AUTO_CLOCK_SLOTS=16, AUTO_CLOCK_POLLS=100000 };
typedef struct auto_clock {
    auto_wire_clock records[AUTO_CLOCK_SLOTS];
    uint32_t instance,epoch,first;
    uint64_t frequency,attempted,lost;
    uint32_t saturated,prepared,published,write_idx,read_idx;
    uint32_t active,last_label,invalid; /* owner only; no nested sample or slot reuse */
} auto_clock;
typedef struct auto_clock_port {
    void *user;
    uint64_t (*tb)(void *); /* ordered around device accesses */
    int (*live)(void *); /* immutable lease/epoch still valid, STOP absent */
    int (*emit)(void *,uint32_t report,uint32_t serial); /* bounded room; no callback */
    void (*flush)(void *); /* original PUT path calls before/after below */
    uint32_t (*label)(void *);
    uint64_t (*timer)(void *,uint32_t report);
    void (*sync)(void *);
} auto_clock_port;
int auto_clock_init(auto_clock *,uint32_t instance,uint32_t epoch,uint32_t first,uint64_t frequency);
/* Owner only, at scheduled boundaries. 1 publishes an immutable success/failure
 * fact; 2 records capacity loss, with no device writes; 0 rejects reentry/args.
 * A whole-sink ACK never makes a clock slot reusable within this capture. */
int auto_clock_sample(auto_clock *,const auto_clock_port *,uint32_t owner,uint64_t frame);
uint32_t auto_clock_before(const auto_clock *); /* before the original PUT */
int auto_clock_after(auto_clock *,uint32_t); /* after original PUT + barrier */
const auto_wire_clock *auto_clock_peek(auto_clock *);
int auto_clock_ack(auto_clock *,uint32_t id,int whole);
#endif
