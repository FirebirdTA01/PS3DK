/* Host device-mapping seam: initial attachment and optional override write no
 * reports, labels or commands. This is not the installed target consumer. */
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "auto_internal.h"
#include "fifo_internal.h"

extern unsigned auto_test_fifo_installs,auto_test_fifo_removes;
extern const ps3tc_fifo_observer *auto_test_fifo_observer;
int p_gpu_fifo_owner(prof *,uint32_t,void *);

static uint64_t reports[2048][2];
static uint32_t labels[256], words[64];
static gpp_ctx context;
static void *report(uint32_t n) { return reports[n]; }
static void *label(uint32_t n) { return &labels[n]; }
static void flush(gpp_ctx *ctx) { (void)ctx; assert(0 && "init must not flush"); }
static const gpp_dev device={report,label,flush};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);

static void *foreign(void *unused)
{
    uint32_t instance=99,domain=99;
    (void)unused;
    assert(ps3dk_prof_thread_begin("foreign-auto") == PS3DK_PROF_OK);
    assert(p_gpu_fifo_owner(g_prof,g_fifo.domain,&context)==PS3DK_PROF_EPERM);
    assert(ps3dk_prof_auto_attach((void *)&context,NULL,&instance,&domain)==PS3DK_PROF_EPERM);
    assert(instance==0 && domain==0);
    ps3dk_prof_thread_end();
    return NULL;
}

int main(void)
{
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    uint32_t instance=0,domain=0;
    memset(reports,0xa5,sizeof reports);memset(labels,0x5a,sizeof labels);
    context=(gpp_ctx){words,words+64,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    assert(g_prof->auto_lease.ctx==&context);
    assert(g_prof->auto_lease.first==1024 && g_prof->auto_lease.count==1024);
    assert(auto_test_fifo_installs==1 && auto_test_fifo_observer && g_fifo.state==FS_ATTACHED);
    assert(g_fifo.domain==g_prof->auto_lease.domain && g_fifo.owner_inc==g_tls.incarnation);
    assert(p_gpu_fifo_owner(g_prof,g_fifo.domain,&context)==0);
    assert(p_gpu_fifo_owner(g_prof,g_fifo.domain,words)==PS3DK_PROF_EINVAL);
    {
        pthread_t t;
        assert(pthread_create(&t,NULL,foreign,NULL)==0);
        assert(pthread_join(t,NULL)==0);
        assert(auto_test_fifo_installs==1 && !auto_test_fifo_removes);
    }
    o.size=0;
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_EINVAL);
    assert(!instance && !domain && g_prof->auto_lease.first==1024);
    o.size=sizeof o;o.report_first=64;o.report_count=32;o.metadata_records=4;o.frame_records=2;
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
    assert(instance==1 && domain==1026 && g_prof->gpu_lifetime==1);
    assert(g_prof->auto_lease.first==64 && g_prof->auto_lease.count==32);
    {
        uint64_t need=((uint64_t)sizeof(auto_capture)+127u)&~UINT64_C(127);
        uint32_t retained=g_prof->auto_lease.ticket;
        need=(need+4*sizeof(auto_queue_entry)+127u)&~UINT64_C(127);
        need=(need+2*sizeof(auto_wire_frame)+127u)&~UINT64_C(127);
        assert(need<2097152 && need>0);
        o.max_bytes=(uint32_t)need-1;
        assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_EINVAL);
        assert(!instance && !domain && g_prof->auto_lease.ticket==retained);
        o.max_bytes=(uint32_t)need;
        ps3dk_prof_test_hooks.alloc_fail=1;
        assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_ENOMEM);
        ps3dk_prof_test_hooks.alloc_fail=0;
        assert(!instance && !domain && g_prof->auto_lease.ticket==retained);
        assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
        assert(instance==1 && domain==1026 && g_prof->gpu_lifetime==1);
    }
    for(unsigned i=0;i<2048;i++) assert(reports[i][0]==UINT64_C(0xa5a5a5a5a5a5a5a5));
    for(unsigned i=0;i<256;i++) assert(labels[i]==UINT32_C(0x5a5a5a5a));
    assert(context.current==words);
    assert(auto_test_fifo_installs==1 && !auto_test_fifo_removes);
    assert(g_fifo.domain==domain);
    {
        /* First wrap closes the override window, without a draw or device
         * command. These are host boundary facts, not observed GPU progress. */
        ps3tc_fifo_obs_state state={0};
        ps3tc_fifo_obs_wrap wrap={4096,8188,16,0};
        state.reg=*auto_test_fifo_observer;
        for(unsigned kind=1;kind<=6;kind++) {
            uint64_t tick=p_tb();
            ps3tc_fifo_obs_emit(&state,kind,tick,tick,&wrap);
        }
        assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_ESTOPPED);
        assert(instance==0 && domain==0 && g_fifo.attempted==6);
    }
    uint64_t drains=ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_DRAINS);
    ps3dk_prof_thread_end();
    assert(!auto_test_fifo_observer && auto_test_fifo_removes==1);
    /* Ordinary retirement must finish before destructor shutdown. A counter
     * published after each complete drain avoids reading live slot fields. */
    unsigned spins;
    for(spins=0;spins<10000;spins++) {
        if(ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_DRAINS)>=drains+3)break;
        pal_sleep_us(1000);
    }
    assert(spins<10000);
    puts("AUTO initial attach and override without device writes PASS");
    return 0;
}
