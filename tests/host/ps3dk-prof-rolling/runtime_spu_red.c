/* Runner-only behavioral RED. Never link into the implemented acceptance gate. */
#include "prof_internal.h"
#include "runtime_spu_test_api.h"
int ps3dk_prof_test_rolling_combined_set(const ps3dk_prof_test_rolling_options *o,const pfm_config *c)
{
    if(!o)return PS3DK_PROF_EINVAL;
    if(o->size==sizeof *o&&o->version==1&&!o->enabled&&!o->reserved)
        return ps3dk_prof_test_rolling_set(o);
    (void)c;return PS3DK_PROF_ENOTSUP;
}
