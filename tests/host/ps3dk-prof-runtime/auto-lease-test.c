/* Real manual GPU attach and AUTO reservation share the extension lock. Fake
 * mapped reports only: no GPU execution, target ordering or retirement claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_internal.h"
#include "prof_gpu.h"
#include "auto_lease.h"

static uint64_t reports[2048][2];
static uint32_t labels[256], words[3][64];
static gpp_ctx contexts[3];
static void *report(uint32_t n) { return reports[n]; }
static void *label(uint32_t n) { return &labels[n]; }
static void flush(gpp_ctx *ctx) { (void)ctx; }
static const gpp_dev device={report,label,flush};
static void lock(void) { assert(p_ext_lock_at(g_prof)==0); }
static void unlock(void) { assert(p_ext_unlock_site(g_prof,U_ANY)==0); }

int main(void)
{
    uint32_t id=0, token=0, old=0, manual=0;
    prof *p=g_prof;
    assert(p && g_tls.slot1);
    for(unsigned i=0;i<3;i++) contexts[i]=(gpp_ctx){words[i],words[i]+64,words[i],NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    lock();
    assert(p_auto_lease_claim(p,&contexts[0],1024,1024,239,238,&id,&token)==0);
    assert(id==1026 && token && p->gpu_lifetime==1);
    old=token;
    unlock();
    /* Same context, either label, and either edge of the report interval. */
    assert(ps3dk_prof_gpu_attach((void *)&contexts[0],0,16,240,&manual)==PS3DK_PROF_EINVAL);
    assert(ps3dk_prof_gpu_attach((void *)&contexts[1],0,16,239,&manual)==PS3DK_PROF_EINVAL);
    assert(ps3dk_prof_gpu_attach((void *)&contexts[1],0,16,238,&manual)==PS3DK_PROF_EINVAL);
    assert(ps3dk_prof_gpu_attach((void *)&contexts[1],1023,2,240,&manual)==PS3DK_PROF_EINVAL);
    assert(ps3dk_prof_gpu_attach((void *)&contexts[1],2046,2,240,&manual)==PS3DK_PROF_EINVAL);
    assert(!manual);
    /* Adjacent manual allocation succeeds and consumes the same id frontier. */
    assert(ps3dk_prof_gpu_attach((void *)&contexts[1],1008,16,240,&manual)==0);
    assert(manual==1027);
    lock();
    assert(p_auto_lease_claim(p,&contexts[0],1000,32,239,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(!id && !token && p->auto_lease.ticket==old);
    assert(p_auto_lease_claim(p,&contexts[0],1024,16,239,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[0],1024,1024,240,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[0],1024,1024,239,240,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[1],1024,1024,239,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[0],1024,1024,239,239,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[0],UINT32_MAX,18,239,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[0],0,1008,239,238,&id,&token)==0);
    assert(id==1026 && token!=old && p->gpu_lifetime==2);
    assert(!p_auto_lease_drop(p,old)); /* stale rollback cannot remove override */
    assert(p->auto_lease.ticket==token);
    assert(p_auto_lease_drop(p,token));
    assert(!p->auto_lease.ctx);
    /* New reservation sees the still-live manual domain in the reverse order. */
    assert(p_auto_lease_claim(p,&contexts[2],1000,32,239,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[2],0,1008,240,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[1],0,1008,239,238,&id,&token)==PS3DK_PROF_EINVAL);
    assert(p_auto_lease_claim(p,&contexts[2],0,1008,239,238,&id,&token)==0);
    assert(id==1028);
    assert(!p_auto_lease_mark(p,old));
    assert(p_auto_lease_mark(p,token));
    assert(!p_auto_lease_drop(p,token)); /* admitted writes retain the lease */
    assert(p_auto_lease_claim(p,&contexts[2],1024,1024,239,238,&id,&old)==PS3DK_PROF_EBUSY);
    assert(!id && !old);
    unlock();
    /* Isolated registry state, no concurrent users or device side effects. */
    {
        prof isolated={0};
        isolated.gpu_lifetime=GPU_LIFETIME_MAX;
        assert(p_auto_lease_claim(&isolated,&contexts[0],0,18,239,238,&id,&old)==PS3DK_PROF_EFULL);
        assert(!id && !old && !isolated.auto_lease.ctx);
        isolated.gpu_lifetime=0;
        isolated.auto_lease.next_ticket=UINT32_MAX;
        assert(p_auto_lease_claim(&isolated,&contexts[0],0,18,239,238,&id,&old)==PS3DK_PROF_EFULL);
        isolated.auto_lease.next_ticket=0;
        isolated.shutdown_once=1;
        assert(p_auto_lease_claim(&isolated,&contexts[0],0,18,239,238,&id,&old)==PS3DK_PROF_ESTOPPED);
        isolated.shutdown_once=0;isolated.stop_latch=1;
        assert(p_auto_lease_claim(&isolated,&contexts[0],0,18,239,238,&id,&old)==PS3DK_PROF_ESTOPPED);
        isolated.stop_latch=0;isolated.ext_sealed=1;
        assert(p_auto_lease_claim(&isolated,&contexts[0],0,18,239,238,&id,&old)==PS3DK_PROF_ESTOPPED);
        isolated.ext_sealed=0;
        assert(p_auto_lease_claim(&isolated,&contexts[0],0,18,239,238,&id,&old)==0);
        isolated.shutdown_once=1;
        assert(!p_auto_lease_mark(&isolated,old));
        assert(p_auto_lease_drop(&isolated,old)); /* shutdown unwind, never a GPU free */
    }
    puts("AUTO/manual shared reservation PASS");
    return 0;
}
