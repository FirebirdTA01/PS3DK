/* Runner-only behavioral RED. Never linked into product or a GREEN binary. */
#include "prof_internal.h"
#include "roll_runtime_test.h"
int ps3dk_prof_test_rolling_set(const ps3dk_prof_test_rolling_options *o)
{
    if(!o)return PS3DK_PROF_EINVAL;
    if(o->size==sizeof *o&&o->version==1&&!o->enabled&&!o->reserved)return PS3DK_PROF_OK;
    return PS3DK_PROF_ENOTSUP;
}
int ps3dk_prof_test_rolling_result(ps3dk_prof_test_rolling_record *out)
{
    if(!out)return PS3DK_PROF_EINVAL;
    return g_prof?PS3DK_PROF_ENOTSUP:PS3DK_PROF_ENOTINIT;
}
