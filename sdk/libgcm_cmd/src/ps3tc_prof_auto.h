#ifndef PS3TC_PROF_AUTO_H
#define PS3TC_PROF_AUTO_H
#include <stdint.h>
struct _gcmCtxData;
typedef struct ps3tc_auto_call {
    uint32_t kind, mode, start, count, offset, index_type, location, clear_mask;
} ps3tc_auto_call;
/* Safe canonical command seams only. A zero ticket still delegates the
 * application command once; end(0) is inert. The observer may not allocate,
 * flush, poll or invoke a reserve callback in either hook. Its admitted room
 * covers both markers and every original reservation group before begin. */
uint32_t ps3dk_prof__auto_begin(struct _gcmCtxData *, const ps3tc_auto_call *, uint64_t caller);
void ps3dk_prof__auto_end(struct _gcmCtxData *, uint32_t ticket);
/* Snapshot committed metadata before PUT, then release the publication
 * frontier after PUT's full barrier. These hooks emit no GPU commands. */
uint32_t ps3dk_prof__auto_publish_before(struct _gcmCtxData *);
void ps3dk_prof__auto_publish_after(struct _gcmCtxData *, uint32_t frontier);
void ps3dk_prof__auto_context_reset(struct _gcmCtxData *);
/* Public application writes, safe and unsafe forms. kind 1 is a report index,
 * kind 2 a label index. Observation never redirects or suppresses the write. */
void ps3dk_prof__auto_resource_write(struct _gcmCtxData *, uint32_t kind, uint32_t index);
/* cellGcmSegmentBegin/End in the observed build: the context between them is a
 * segment context. Neither hook writes commands or changes the segment. */
void ps3dk_prof__auto_segment_open(struct _gcmCtxData *);
void ps3dk_prof__auto_segment_close(struct _gcmCtxData *);
/* cellGcmSegmentCall in the observed build, after the CALL and the segment's
 * retirement label were written into the ring: the observer appends one
 * operation-label release completing the operations built in that segment.
 * cellGcmSegmentCall reserved those 4 words together with the CALL and the
 * label, so the hook always has them; a BUSY Call never reaches the hook. */
void ps3dk_prof__auto_segment_call(struct _gcmCtxData *ring, struct _gcmCtxData *segment);
void ps3dk_prof__auto_segment_external_open(struct _gcmCtxData *key, const void *buffer, uint32_t size);
void ps3dk_prof__auto_segment_external_close(struct _gcmCtxData *key);
/* gcmSetFlip / gcmSetPrepareFlip in a PS3TC_PROF_AUTO translation unit (rsx/gcm_sys.h),
 * after the flip was queued: ok is nonzero when it was accepted. */
void ps3dk_prof__auto_flip(struct _gcmCtxData *, int32_t ok);
/* gcmGetFlipStatus in a PS3TC_PROF_AUTO unit: begin returns an ordered PPU time
 * (0 when not observed), end receives it with the status the firmware returned. */
uint64_t ps3dk_prof__auto_flip_status_begin(void);
void ps3dk_prof__auto_flip_status_end(uint64_t before, uint32_t status);
/* cellGcmWaitFlipComplete in a PS3TC_PROF_AUTO unit: one flip-wait scope. begin returns
 * a nonzero token when observed; end receives it with the helper's return code. */
uint64_t ps3dk_prof__auto_flip_wait_begin(void);
void ps3dk_prof__auto_flip_wait_end(uint64_t scope, int32_t rc);
#endif
