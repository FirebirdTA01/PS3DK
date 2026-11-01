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
