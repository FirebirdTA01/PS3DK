#ifndef PS3DK_PROF_AUTO_FRAMES_H
#define PS3DK_PROF_AUTO_FRAMES_H
#include "auto_wire.h"
#define auto_frames_init ps3dk_prof__auto_frames_init
#define auto_frames_open ps3dk_prof__auto_frames_open
#define auto_frames_count ps3dk_prof__auto_frames_count
#define auto_frames_close ps3dk_prof__auto_frames_close
#define auto_frames_peek ps3dk_prof__auto_frames_peek
#define auto_frames_ack ps3dk_prof__auto_frames_ack
typedef struct auto_frames {
    auto_wire_frame *entries;
    uint32_t capacity,instance,owner;
    uint64_t write_idx,read_idx,lost;
    uint32_t saturated;
    /* owner only; queued snapshots are immutable until whole sink acceptance */
    uint32_t active,seen;
    uint64_t last;
    auto_wire_frame live;
} auto_frames;
int auto_frames_init(auto_frames *,auto_wire_frame *,uint32_t capacity,uint32_t instance,uint32_t owner);
/* Opens monotonically increasing identities, including frame zero. A previous
 * unclosed frame is emitted OPEN, never silently certified as exactly closed. */
int auto_frames_open(auto_frames *,uint64_t frame,uint64_t tb);
/* Columns match COVERAGE: 0 visit, 1 observed, 2..11 disjoint exclusions.
 * A visit and its outcome must reconcile before closure. No active frame means
 * false, so the caller records an unassociated visit rather than inventing one. */
int auto_frames_count(auto_frames *,uint32_t kind,uint32_t column);
/* 0: refused identity/accounting, unchanged; 1: queued; 2: queue loss recorded.
 * An implied closure or reversed clock is OPEN. Actual TB observations survive
 * a reversed clock; no replacement timestamp is invented. lost is cumulative. */
int auto_frames_close(auto_frames *,uint32_t owner,uint64_t frame,uint64_t tb,int open);
const auto_wire_frame *auto_frames_peek(auto_frames *);
int auto_frames_ack(auto_frames *,uint32_t owner,uint64_t frame,int whole);
#endif
