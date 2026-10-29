#include "eye_capture.h"
#include <string.h>

static void failure(EyeStats *s, enum EyeStage stage, int rc)
{
    if (!s->first_error) { s->first_error=rc; s->error_stage=stage; }
}

int eye_capture_run(const EyeOps *ops, unsigned target, uint64_t budget_us, EyeStats *s)
{
    if (!s) return EYE_BAD_ARGUMENT;
    memset(s,0,sizeof(*s));
    if (!ops || !ops->control || !ops->read || !ops->present || !ops->pump ||
        !ops->now || !ops->wait || !target || !budget_us) {
        failure(s,EYE_FRAME,EYE_BAD_ARGUMENT);
        return s->first_error;
    }
    unsigned acquired=0;
    int quit=0;
    uint32_t previous=0;
    uint64_t begin=ops->now(ops->context), last_fresh=begin;
    for (int stage=EYE_LOAD; stage<=EYE_START; ++stage) {
        int rc=ops->control(ops->context,(enum EyeStage)stage);
        if (rc) { failure(s,(enum EyeStage)stage,rc); goto cleanup; }
        ++acquired;
    }
    last_fresh=ops->now(ops->context);
    while (s->displayed < target && !s->first_error) {
        int rc=ops->pump(ops->context);
        if (rc==1) { quit=1; break; }
        if (rc) { failure(s,EYE_PUMP,rc); break; }
        uint64_t now=ops->now(ops->context);
        if (now < begin || now < last_fresh || now-begin >= budget_us ||
            now-last_fresh >= EYE_FRAME_TIMEOUT_US) {
            failure(s,EYE_DEADLINE,EYE_TIMEOUT); break;
        }
        EyeFrame frame={0};
        rc=ops->read(ops->context,&frame);
        if (rc==1) { ops->wait(ops->context); continue; }
        if (rc) { failure(s,EYE_READ,rc); break; }
        if (!frame.data || !frame.bytes) { failure(s,EYE_FRAME,EYE_INVALID_FRAME); break; }
        if (s->fresh && frame.number==previous) {
            ++s->duplicates; ops->wait(ops->context); continue;
        }
        if (s->fresh && ((uint32_t)(frame.number-previous)>INT32_MAX ||
                         frame.timestamp<s->last_timestamp)) {
            failure(s,EYE_FRAME,EYE_INVALID_FRAME); break;
        }
        now=ops->now(ops->context);
        if (now < begin || now-begin>=budget_us || now-last_fresh>=EYE_FRAME_TIMEOUT_US) {
            failure(s,EYE_DEADLINE,EYE_TIMEOUT); break;
        }
        if (!s->fresh) s->first_timestamp=frame.timestamp;
        s->last_timestamp=frame.timestamp;
        previous=frame.number;
        ++s->fresh;
        last_fresh=now;
        rc=ops->present(ops->context,&frame);
        if (rc) { failure(s,EYE_PRESENT,rc); break; }
        ++s->displayed;
        now=ops->now(ops->context);
        if (now < begin || now-begin>=budget_us) failure(s,EYE_DEADLINE,EYE_TIMEOUT);
        /* Presentation may have drained a flip after observing user exit.
         * Poll even after the final frame so that interruption stays visible. */
        rc=ops->pump(ops->context);
        if (rc==1) { quit=1; break; }
        if (rc) { failure(s,EYE_PUMP,rc); break; }
    }
cleanup:
    /* Acquisition index 3 (start) pairs with stop 4, then open/close etc. */
    while (acquired) {
        enum EyeStage stage=(enum EyeStage)(7-(--acquired));
        int rc=ops->control(ops->context,stage);
        if (rc) { ++s->cleanup_failures; failure(s,stage,rc); }
    }
    uint64_t end=ops->now(ops->context);
    s->elapsed_us=end>=begin ? end-begin : 0;
    return s->first_error ? s->first_error : quit ? 1 : 0;
}
