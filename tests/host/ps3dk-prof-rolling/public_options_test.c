/* Public contract against the real private planner; no allocator or target IO. */
#include <ps3dk/prof_rolling.h>
#include "roll_runtime.h"
#include "roll_public.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(ps3dk_prof_rolling_options)==192,"options ABI");
_Static_assert(offsetof(ps3dk_prof_rolling_options,max_capture_bytes)==16,"file ABI");
_Static_assert(offsetof(ps3dk_prof_rolling_options,rsx_domains)==112,"AUTO ABI");
_Static_assert(offsetof(ps3dk_prof_rolling_options,reserved)==152,"reserved ABI");
_Static_assert(sizeof(ps3dk_prof_rolling_result)==128,"result ABI");
_Static_assert(offsetof(ps3dk_prof_rolling_result,result)==88,"error ABI");
static void refuses(ps3dk_prof_rolling_options q)
{
    p_roll_options o, before; p_roll_selection s, prior;
    memset(&o,0xa5,sizeof o);memset(&s,0xa5,sizeof s);before=o;prior=s;
    assert(ps3dk_prof__roll_options_decode(&q,&o,&s)==PS3DK_PROF_EINVAL);
    assert(!memcmp(&o,&before,sizeof o)&&!memcmp(&s,&prior,sizeof s));
}
int main(void)
{
    ps3dk_prof_rolling_options q=PS3DK_PROF_ROLLING_AUTO_FIFO_INIT(UINT64_C(67108864),UINT64_C(67108864));
    p_roll_options o; p_roll_selection s;size_t n; pfm2_layout e;
    assert(ps3dk_prof__roll_options_decode(&q,&o,&s)==0);
    assert(s.kind==P_ROLL_SCHEMA2&&s.limits.schema2.family_flags==192);
    assert(o.store.max_bytes==q.max_capture_bytes&&o.max_state_bytes==q.max_fixed_state_bytes);
    assert(s.limits.schema2.shared.metadata_bytes==q.metadata_bytes);
    assert(s.limits.schema2.shared.epochs>0&&s.limits.schema2.shared.journal_entries>0);
    assert(!s.limits.schema2.shared.spu_domains&&!s.limits.schema2.shared.spu_lanes);
    assert(pfm2_plan(&s.limits.schema2,&e)==0);
    assert(p_roll_plan(&o,&s,&n)==0&&n>0&&n<=q.max_fixed_state_bytes);
    printf("AUTO preset private allocation=%zu checkpoint_bound=%zu\n",n,e.checkpoint_bound);
    {ps3dk_prof_rolling_options bad=q;bad.size--;refuses(bad);bad=q;bad.version++;refuses(bad);
     bad=q;bad.reserved[9]=1;refuses(bad);bad=q;bad.mode=3;refuses(bad);
     bad=q;bad.profile=99;refuses(bad);bad=q;bad.metadata_bytes=15;refuses(bad);
     bad=q;bad.prepare_work_limit=0;refuses(bad);bad=q;bad.spu_lanes=1;refuses(bad);}
    q=(ps3dk_prof_rolling_options)PS3DK_PROF_ROLLING_OPTIONS_INIT;
    assert(ps3dk_prof__roll_options_decode(&q,&o,&s)==0&&!o.enabled&&s.kind==P_ROLL_NONE);
    q.max_capture_bytes=1;refuses(q);
    puts("public options ABI, finite AUTO preset and transactional refusal PASS");
    return 0;
}
