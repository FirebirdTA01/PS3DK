#include "workload.h"
#include "spu_pool.h"
#include "scene.h"
#include <ps3dk/prof.h>
#include <sys/process.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
SYS_PROCESS_PARAM(1001,0x100000);
static const char *names[4]={"PPU serial / light RSX","SPE parallel / light RSX",
                            "SPE parallel / heavy RSX","PPU serial / heavy RSX"};
int main(void)
{
    int rc=1,scene_owned=0,pool_owned=0,prof_owned=0;
    unsigned checked=0,phase_seen=0,frames=0;
    uint32_t sequence=0;
    uint64_t gpu_begin=0,gpu_end=0,frame_us=0;
    int gpu_valid=0;
    char path[160];
    snprintf(path,sizeof path,"/dev_hdd0/tmp/heterogeneous-%016llx.csv",(unsigned long long)sample_now_us());
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0){printf("WORKLOAD_FAIL log-open errno=%d\n",errno);return 1;}
    FILE *log=fdopen(fd,"w");
    if(!log){close(fd);return 1;}
    fprintf(log,"kind,frame,phase,sequence,lane,items,digest,compute_us,frame_us,spu_dec_begin,spu_dec_end,gpu_begin,gpu_end,gpu_valid\n");
    printf("WORKLOAD_BEGIN abi=%s profiling=%d csv=%s phase_seconds=%u session_seconds=%u\n",
           SAMPLE_ABI,PS3DK_PROF,path,(unsigned)SAMPLE_PHASE_SECONDS,(unsigned)SAMPLE_SESSION_SECONDS);
    scene_owned=1;
    if(scene_init()){puts("WORKLOAD_FAIL scene-init");goto done;}
    uint64_t idle=sample_now_us();
    while(sample_now_us()-idle<(uint64_t)SAMPLE_IDLE_SECONDS*1000000){
        int poll=scene_poll();if(poll<0)goto done;if(poll>0){rc=0;goto done;}
        if(scene_render(0,"No-input idle baseline",0,0,0,&gpu_begin,&gpu_end,&gpu_valid))goto done;
    }
    if(pool_init()){puts("WORKLOAD_FAIL no usable SPE pool");goto done;}
    pool_owned=1;
    printf("WORKLOAD_POOL workers=%u\n",pool_count());
#if PS3DK_PROF
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.app="heterogeneous";cfg.build_id=SAMPLE_BUILD_ID;cfg.workload_tag="ppu-spe-rsx-loop";
    cfg.mode=PS3DK_PROF_MODE_APP;cfg.max_threads=1;cfg.max_descriptors=64;cfg.ring_records=512;
    if(ps3dk_prof_set_sink_path("/dev_hdd0/tmp") || PS3DK_PROF_INIT(&cfg)){puts("WORKLOAD_FAIL profiler-init");goto done;}
    prof_owned=1;
    if(PS3DK_PROF_THREAD_BEGIN("main")){puts("WORKLOAD_FAIL profiler-thread");goto done;}
#endif
    uint64_t start=sample_now_us();int previous=-1;
    for(;;){
        uint64_t begin=sample_now_us(),elapsed=begin-start;
        int poll=scene_poll();if(poll<0)goto done;if(poll>0)break;
        if(SAMPLE_SESSION_SECONDS && elapsed>=(uint64_t)SAMPLE_SESSION_SECONDS*1000000)break;
        int phase=work_phase(elapsed,(uint64_t)SAMPLE_PHASE_SECONDS*1000000);
        if(phase!=previous){
            printf("WORKLOAD_PHASE index=%d name=%s\n",phase,names[phase]);
            phase_seen|=1u<<phase;previous=phase;
            if(fflush(log)){puts("WORKLOAD_FAIL log-flush");goto done;}
        }
        PS3DK_PROF_FRAME();
        if(sequence==UINT32_MAX){puts("WORKLOAD_FAIL sequence-limit");goto done;}
        ++sequence;
        work_result result={0,0};uint64_t compute_begin=sample_now_us();
        if(phase==0 || phase==3){
            PS3DK_PROF_ZONE("compute.ppu_serial");
            result=work_partition(0,1,WORK_ITEMS);
        }else{
            {
                PS3DK_PROF_ZONE("compute.spe_dispatch");
                if(pool_submit(sequence)){puts("WORKLOAD_FAIL dispatch");goto done;}
            }
            {
                PS3DK_PROF_ZONE("compute.spe_wait");
                while(!pool_ready(sequence)){
                    if(scene_poll()<0 || sample_now_us()-compute_begin>=2000000){puts("WORKLOAD_FAIL SPE deadline");goto done;}
                    usleep(100);
                }
            }
            for(unsigned i=0;i<pool_count();++i){
                const volatile work_slot *s=pool_slot(i);
                result.digest^=s->digest;result.count+=s->count;
            }
        }
        uint64_t compute_us=sample_now_us()-compute_begin;
        if(result.count!=WORK_ITEMS || result.digest!=WORK_DIGEST){puts("WORKLOAD_FAIL compute-result");goto done;}
        ++checked;
        if(scene_render(phase>=2,names[phase],pool_count(),compute_us,frame_us,&gpu_begin,&gpu_end,&gpu_valid)){
            puts("WORKLOAD_FAIL render/present");goto done;
        }
        frame_us=sample_now_us()-begin;
        /* Export after timing; these buffered writes are not SPE compute cost.
         * The next dispatch cannot reuse worker slots until this loop ends. */
        if(phase==1 || phase==2)for(unsigned i=0;i<pool_count();++i){
            const volatile work_slot *s=pool_slot(i);
            fprintf(log,"SPE,%u,%d,%u,%u,%u,%08x,0,0,%u,%u,0,0,0\n",frames,phase,sequence,i,s->count,s->digest,s->begin_dec,s->end_dec);
        }
        fprintf(log,"FRAME,%u,%d,%u,0,%u,%08x,%llu,%llu,0,0,%llu,%llu,%d\n",frames,phase,sequence,result.count,result.digest,
                (unsigned long long)compute_us,(unsigned long long)frame_us,(unsigned long long)gpu_begin,(unsigned long long)gpu_end,gpu_valid);
        ++frames;
        if(PS3DK_PROF_DRAIN()!=0 || ferror(log)){puts("WORKLOAD_FAIL capture/log");goto done;}
    }
    rc=0;
done:
    if(pool_owned && pool_close()!=0)rc=1;
    if(prof_owned && PS3DK_PROF_SHUTDOWN()!=0)rc=1;
    if(scene_owned)scene_close();
    fprintf(log,"END,%u,%u,0,0,%u,%d,0,0,0,0,0,0,0\n",frames,phase_seen,checked,rc);
    if(fflush(log) || fsync(fileno(log)))rc=1;
    if(fclose(log))rc=1;
    printf("WORKLOAD_END frames=%u checked=%u phase_mask=%u rc=%d\n",frames,checked,phase_seen,rc);
    return rc;
}
