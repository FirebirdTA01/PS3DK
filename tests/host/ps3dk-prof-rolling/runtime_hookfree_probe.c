/* Compile-only internal contract. No public setter, executable or runtime enable. */
#include "prof_internal.h"
#include <ps3dk/prof_rolling.h>
_Static_assert(sizeof(ps3dk_prof_rolling_options)==192,"public options size on both ABIs");
_Static_assert(offsetof(ps3dk_prof_rolling_options,max_capture_bytes)==16,"public options alignment");
_Static_assert(offsetof(ps3dk_prof_rolling_options,reserved)==152,"public reserved offset");
_Static_assert(sizeof(ps3dk_prof_rolling_result)==128,"public result size on both ABIs");
_Static_assert(offsetof(ps3dk_prof_rolling_result,result)==88,"public result error offset");
#ifndef P_ROLL_EXPECT_POINTER_BYTES
#error "compile driver must name the expected pointer ABI"
#endif
_Static_assert(sizeof(void *)==P_ROLL_EXPECT_POINTER_BYTES,"target pointer ABI");
_Static_assert(sizeof(size_t)==P_ROLL_EXPECT_POINTER_BYTES,"target size_t ABI");
#ifdef P_ROLL_EXPECT_ENABLED
#ifndef P_ROLLING_RUNTIME
#error "hook-free internal runtime declarations required"
#else
_Static_assert(sizeof(((p_roll_runtime *)0)->options.max_state_bytes)==8,"internal memory bound width");
_Static_assert(sizeof(((p_roll_runtime *)0)->record.accepted_next_chunk)==8,"accepted frontier width");
_Static_assert(sizeof(((p_roll_runtime *)0)->record.committed_payload_bytes)==8,"committed byte width");
int ps3dk_prof__hookfree_compile_calls(prof *p)
{
    size_t bytes=0;
    int rc=p_roll_plan(NULL,NULL,&bytes);
    p_roll_bind(p,NULL,NULL,NULL,bytes);
    rc+=p_roll_start(p,NULL);
    rc+=p_roll_accept(p,NULL,0);
    rc+=p_roll_flush(p);
    rc+=p_roll_finish(p);
    p_roll_publish(p,rc);
    return rc;
}
#endif
#else
#ifdef P_ROLLING_RUNTIME
#error "ordinary gate-off must not expose internal runtime declarations"
#endif
int ps3dk_prof__hookfree_compile_disabled(void){return 0;}
#endif
