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
/* Exact owner FIFO IO offset; independent of AUTO report publication serials. */
void ps3dk_prof__fifo_account_native_publish(void *ctx, uint32_t offset);
/* Report ring (owner contexts only; inert otherwise). prewrap: the native FIFO
 * wrap, before it computes the tail JUMP from current. preflush: FlushBuffer,
 * before its barrier and before current becomes PUT. Either may append a
 * bounded block flush at current inside the tail it reserved by lowering end,
 * and restores that exact physical end; neither calls a callback, flushes or
 * waits. Callers re-read current and end afterwards; nothing cached survives. */
void ps3dk_prof__auto_prewrap(struct _gcmCtxData *);
void ps3dk_prof__auto_preflush(struct _gcmCtxData *);
/* FIFO accounting (owner thread): 1 when visible_end is a known end of ctx: equal to the registered physical end, or
 * exactly the end the report ring's active tail lowered that physical end to on the owner context it attached to.
 * The same predicate as the runtime's own clean-end check; a physical end is never derived by subtraction. */
int ps3dk_prof__auto_fifo_end_known(void *ctx, const void *registered_physical_end, const void *visible_end);
/* Provided by the observed ps3tc_fifo_wrap.o: 1 when the context's callback is
 * the native two-phase wrap that calls prewrap; any replaced or custom callback
 * would wrap without it, so recycling refuses. */
int ps3tc_fifo_wrap_native(const struct _gcmCtxData *);
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
/* cellGcmSegmentCall in the observed build, after its room check and before the
 * CALL: the observer may write a 2-word timestamp report (the begin of a
 * whole-CALL bracket, ended by ps3dk_prof__auto_segment_call). Call reserved
 * those words too, with the 2 of the bracket's end report. */
void ps3dk_prof__auto_segment_call_begin(struct _gcmCtxData *ring, struct _gcmCtxData *segment);
/* cellGcmSetFragmentProgram in a PS3TC_PROF_AUTO unit (cell/gcm/gcm_cg_bridge.h): the offset just bound. */
void ps3dk_prof__auto_fragment_program(struct _gcmCtxData *context, uint32_t offset);
/* RSX frame capture: the program container's ucode bound at an FP offset (rsxLoadFragmentProgramLocation), called
 * only while ps3dk_prof__rsxf_active is non-NULL (the capture selected and attached with the report ring). */
extern void *ps3dk_prof__rsxf_active;
void ps3dk_prof__rsxf_fp_bind(struct _gcmCtxData *context, const void *ucode, uint32_t bytes, uint32_t offset);
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
