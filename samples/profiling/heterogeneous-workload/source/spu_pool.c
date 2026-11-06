#include "spu_pool.h"
#include <sys/spu.h>
#include <stdio.h>
#include <string.h>
#include "workload_spu_bin.h"
static volatile work_slot slots[WORK_MAX_SPUS];
static sysSpuImage image;
static uint32_t group,threads[WORK_MAX_SPUS];
static unsigned workers;
static int image_owned,group_owned,running;
_Static_assert(sizeof(work_slot)==256,"DMA payload and commit separation");
int pool_close(void)
{
    if(group_owned) {
        if(running) {
            /* Termination, rather than a payload commit, establishes that the
             * persistent workers are stopped before the blocking join. */
            int rc=sysSpuThreadGroupTerminate(group,0);
            if(rc){printf("POOL terminate=%08x\n",(unsigned)rc);return -1;}
            uint32_t cause=0,status=0;
            rc=sysSpuThreadGroupJoin(group,&cause,&status);
            if(rc){printf("POOL join=%08x\n",(unsigned)rc);return -1;}
            running=0;
        }
        int rc=sysSpuThreadGroupDestroy(group);
        if(rc){printf("POOL destroy=%08x\n",(unsigned)rc);return -1;}
        group_owned=0;
    }
    if(image_owned) {
        int rc=sysSpuImageClose(&image);
        if(rc){printf("POOL image-close=%08x\n",(unsigned)rc);return -1;}
        image_owned=0;
    }
    workers=0;return 0;
}
int pool_init(void)
{
    int rc=sysSpuInitialize(WORK_MAX_SPUS,0);
    if(rc){printf("POOL initialize=%08x\n",(unsigned)rc);return -1;}
    rc=sysSpuImageImport(&image,workload_spu_bin,0);
    if(rc){printf("POOL import=%08x\n",(unsigned)rc);return -1;}
    image_owned=1;
    for(workers=WORK_MAX_SPUS;workers;--workers) {
        sysSpuThreadGroupAttribute attr={0};
        attr.name="prof-workers";attr.nsize=13;
        rc=sysSpuThreadGroupCreate(&group,workers,100,&attr);
        if(!rc){group_owned=1;break;}
        printf("POOL unavailable workers=%u rc=%08x\n",workers,(unsigned)rc);
    }
    if(!workers){pool_close();return -1;}
    memset((void *)slots,0,sizeof slots);
    for(unsigned i=0;i<workers;++i) {
        sysSpuThreadAttribute attr={0};attr.name="prof-worker";attr.nsize=12;
        sysSpuThreadArgument args={0};
        args.arg0=(uint64_t)(uintptr_t)&slots[i];args.arg1=i;args.arg2=workers;
        rc=sysSpuThreadInitialize(&threads[i],group,i,&image,&attr,&args);
        if(rc){printf("POOL thread-init=%08x\n",(unsigned)rc);pool_close();return -1;}
    }
    /* Fixed immutable job parameters are syscall arguments; the only per-job
     * command is its mailbox sequence, so no shared command region is reused. */
    __asm__ volatile("sync" ::: "memory");
    rc=sysSpuThreadGroupStart(group);
    if(rc){printf("POOL start=%08x\n",(unsigned)rc);pool_close();return -1;}
    running=1;return 0;
}
unsigned pool_count(void){return workers;}
int pool_submit(uint32_t sequence)
{
    if(!running || !sequence)return -1;
    __asm__ volatile("sync" ::: "memory");
    for(unsigned i=0;i<workers;++i) {
        int rc=sysSpuThreadWriteMb(threads[i],sequence);
        if(rc){printf("POOL mailbox=%08x\n",(unsigned)rc);return -1;}
    }
    return 0;
}
int pool_ready(uint32_t sequence)
{
    for(unsigned i=0;i<workers;++i)if(slots[i].sequence!=sequence)return 0;
    __asm__ volatile("sync" ::: "memory");
    return 1;
}
const volatile work_slot *pool_slot(unsigned lane){return &slots[lane];}

#if SAMPLE_PROF_EXT
/* SAMPLE_PROF_EXT: the protocol is pool_proto.c (host-tested); this block binds its port to the LV2 SPU syscalls
 * and the work slots, and owns the epoch state. */
#include "prof_ext_rt.h"
#include "pool_proto.h"
#include <sys/sys_time.h>
#include <unistd.h>
static pe_epochs epochs;
static int io_write_mb(void *c,unsigned i,uint32_t w){(void)c;return sysSpuThreadWriteMb(threads[i],w);}
static uint32_t io_seq(void *c,unsigned i){(void)c;return slots[i].sequence;}
static uint32_t io_status(void *c,unsigned i){(void)c;return slots[i].reserved[0];}
static void io_barrier(void *c){(void)c;__asm__ volatile("sync" ::: "memory");}
static uint64_t io_now(void *c){(void)c;return (uint64_t)sys_time_get_system_time();}
static void io_sleep(void *c,unsigned us){(void)c;usleep(us);}
static int io_join(void *c)
{
    (void)c;
    uint32_t cause=0,status=0;
    int rc=sysSpuThreadGroupJoin(group,&cause,&status);
    if(!rc)running=0; /* the close below then skips the terminate */
    return rc;
}
static int io_close(void *c){(void)c;return pool_close();}
static void io_log(void *c,const char *what,int v){(void)c;printf("POOL %s %d\n",what,v);}
static const pool_io io={0,io_write_mb,io_seq,io_status,io_barrier,io_now,io_sleep,io_join,io_close,io_log};
int pool_create(void)
{
    int rc=sysSpuInitialize(WORK_MAX_SPUS,0);
    if(rc){printf("POOL initialize=%08x\n",(unsigned)rc);return -1;}
    rc=sysSpuImageImport(&image,workload_spu_bin,0);
    if(rc){printf("POOL import=%08x\n",(unsigned)rc);return -1;}
    image_owned=1;
    for(workers=WORK_MAX_SPUS;workers;--workers) {
        sysSpuThreadGroupAttribute attr={0};
        attr.name="prof-workers";attr.nsize=13;
        rc=sysSpuThreadGroupCreate(&group,workers,100,&attr);
        if(!rc){group_owned=1;break;}
        printf("POOL unavailable workers=%u rc=%08x\n",workers,(unsigned)rc);
    }
    if(!workers){pool_close();return -1;}
    memset((void *)slots,0,sizeof slots);
    return 0;
}
int pool_start_ext(void)
{
    int rc;
    for(unsigned i=0;i<workers;++i) {
        sysSpuThreadAttribute attr={0};attr.name="prof-worker";attr.nsize=12;
        sysSpuThreadArgument args={0};
        const pe_lane *l=pe_rt_lane(i);
        pe_args a;
        if(!l || pe_pack_ext((uint32_t)(uintptr_t)&slots[i],i,workers,l->ea_ring,l->ring_slots,l->ea_ctl,&a))
            pe_pack_ext((uint32_t)(uintptr_t)&slots[i],i,workers,0,1,0,&a); /* no lane: the worker runs unprofiled */
        args.arg0=a.arg0;args.arg1=a.arg1;args.arg2=a.arg2;args.arg3=a.arg3;
        rc=sysSpuThreadInitialize(&threads[i],group,i,&image,&attr,&args);
        if(rc){printf("POOL thread-init=%08x\n",(unsigned)rc);pool_close();return -1;}
    }
    __asm__ volatile("sync" ::: "memory");
    pe_epochs_init(&epochs,workers,io_now(0)); /* before the start: an early bound for the attach-time reset */
    if(!pe_rt_spu_on())pe_clear(&epochs,"SPU lanes are off");
    rc=sysSpuThreadGroupStart(group);
    if(rc){printf("POOL start=%08x\n",(unsigned)rc);pool_close();return -1;}
    running=1;return 0;
}
int pool_submit_ext(uint32_t sequence){return running?pp_submit(&epochs,&io,workers,sequence):-1;}
void pool_done_ext(void){pp_done(&epochs,&io,workers);}
void pool_failed_ext(const char *why){pp_failed(&epochs,why);}
int pool_keepalive(uint32_t sequence){return running?pp_keepalive(&epochs,&io,workers,sequence):-1;}
int pool_close_ext(int *join_ok){return pp_close(&epochs,&io,workers,running,join_ok);}
int pool_w32_eligible(const char **why){if(why)*why=epochs.why;return epochs.w32_eligible;}
#endif
