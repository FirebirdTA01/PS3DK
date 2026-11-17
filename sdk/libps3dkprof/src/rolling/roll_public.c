/* Public fixed-width fields translated to private, checked planner inputs. */
#include "roll_public.h"
#include "roll_runtime.h"
#include <string.h>
_Static_assert(sizeof(ps3dk_prof_rolling_options)==192,"rolling options ABI");
_Static_assert(sizeof(ps3dk_prof_rolling_result)==128,"rolling result ABI");
int ps3dk_prof__roll_options_decode(const ps3dk_prof_rolling_options *q,p_roll_options *out,p_roll_selection *selected)
{
    p_roll_options o={0};p_roll_selection s={0};pfm2_config *a=&s.limits.schema2;
    pfm_config *m=&a->shared;unsigned i;size_t n;
    if(!q||!out||!selected||q->size!=192||q->version!=1)return PS3DK_PROF_EINVAL;
    for(i=0;i<10;i++)if(q->reserved[i])return PS3DK_PROF_EINVAL;
    o.size=sizeof o;o.version=1;
    if(q->mode==PS3DK_PROF_ROLLING_APPEND) {
        ps3dk_prof_rolling_options empty=PS3DK_PROF_ROLLING_OPTIONS_INIT;
        if(memcmp(q,&empty,sizeof empty))return PS3DK_PROF_EINVAL;
        *out=o;*selected=s;return 0;
    }
    if(q->mode!=PS3DK_PROF_ROLLING_ENABLED||q->profile<1||q->profile>3)return PS3DK_PROF_EINVAL;
    /* First production profile is managed AUTO+FIFO. Manual PPU/SPU remain
     * private until their installed archive path has passed its own gate. */
    if(q->profile!=PS3DK_PROF_ROLLING_PROFILE_AUTO_FIFO)return PS3DK_PROF_ENOTSUP;
    if(q->metadata_bytes<16||!q->prepare_work_limit||q->spu_domains||q->spu_lanes||q->spu_images||
       q->brackets||q->image_descriptors||q->event_runs||q->live_spu_stack_rows||q->saved_spu_witnesses)
        return PS3DK_PROF_EINVAL;
    o.enabled=1;o.max_state_bytes=q->max_fixed_state_bytes;o.store.max_bytes=q->max_capture_bytes;
    o.store.retained_slots=q->retained_segments;o.store.checkpoint_bytes=q->checkpoint_bytes;
    o.store.payload_bytes=q->segment_payload_bytes;s.kind=P_ROLL_SCHEMA2;a->family_flags=192;
    m->incarnations=q->incarnations;m->descriptors=q->descriptors;m->ppu_stack_rows=q->ppu_stack_rows;
    m->frame_identities=q->frame_identities;m->epochs=q->epochs;m->journal_entries=q->journal_entries;
    m->metadata_bytes=q->metadata_bytes;m->checkpoint_bytes=q->checkpoint_bytes;m->prepare_work=q->prepare_work_limit;
    a->rsx_domains=q->rsx_domains;a->windows=q->windows;a->fifo_membership_runs=q->fifo_membership_runs;
    a->accounting_groups=q->accounting_groups;a->auto_final_runs=q->auto_final_runs;a->auto_touched_runs=q->auto_touched_runs;
    a->auto_build_entries=q->auto_build_entries;a->auto_clock_anchors=q->auto_clock_anchors;
    a->auto_bootstrap_bytes=q->auto_bootstrap_bytes;a->auto_support_bytes=q->auto_support_bytes;
#ifdef P_ROLLING_RUNTIME
    {int rc=p_roll_plan(&o,&s,&n);if(rc)return rc;}
#else
    (void)n;return PS3DK_PROF_ENOTSUP;
#endif
    *out=o;*selected=s;return 0;
}
