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
#if SAMPLE_LIVE
#include "live.h"
#endif
#if SAMPLE_PROF_EXT
#include "prof_ext_rt.h"
#if !PS3DK_PROF
#error "SAMPLE_PROF_EXT needs SAMPLE_PROFILING"
#endif
/* The receipt (prof_ext.h): a loader-launched run has no captured stdout.  Every step is checked (pe_receipt_write). */
static int r_open(void *c,const char *p){(void)c;return open(p,O_WRONLY|O_CREAT|O_APPEND,0600);}
static int r_open_trunc(void *c,const char *p){(void)c;return open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);}
static long r_write(void *c,int fd,const void *b,unsigned long n){(void)c;return (long)write(fd,b,n);}
static int r_fsync(void *c,int fd){(void)c;return fsync(fd);}
static int r_close(void *c,int fd){(void)c;return close(fd);}
static int r_rename(void *c,const char *a,const char *b){(void)c;return rename(a,b);}
static int r_unlink(void *c,const char *p){(void)c;return unlink(p);}
static const pe_file_io receipt_io={0,r_open,r_open_trunc,r_write,r_fsync,r_close,r_rename,r_unlink};
/* start notes: appended to <csv stem>.ext.txt (informational, never an acceptance record) */
static void ext_note(const char *csv_path,const char *line)
{
    char p[176];
    fputs(line,stdout);
    if(pe_receipt_path(csv_path,".ext.txt",p,sizeof p)!=0 || pe_receipt_write(&receipt_io,p,line)!=0)
        printf("WORKLOAD_PROF_EXT note write FAILED\n");
}
/* the acceptance record: published to <csv stem>.ext-end.txt only after a complete write, fsync and close */
static int ext_publish_end(const char *csv_path,const char *line)
{
    char tmp[176],fin[176];
    fputs(line,stdout);
    if(pe_receipt_path(csv_path,".ext-end.tmp",tmp,sizeof tmp)!=0 || pe_receipt_path(csv_path,".ext-end.txt",fin,sizeof fin)!=0)
        return -1;
    return pe_receipt_publish(&receipt_io,tmp,fin,line);
}
#endif
SYS_PROCESS_PARAM(1001,0x100000);
static const char *names[4]={"PPU serial / light RSX","SPE parallel / light RSX",
                            "SPE parallel / heavy RSX","PPU serial / heavy RSX"};
int main(void)
{
    int rc=1,scene_owned=0,pool_owned=0,prof_owned=0;
#if SAMPLE_PROF_EXT
    int ext_ran=0,ext_join=0,ext_eligible=0;const char *ext_why=NULL;
#endif
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
    printf("WORKLOAD_BEGIN abi=%s profiling=%d live=%d csv=%s phase_seconds=%u session_seconds=%u\n",
           SAMPLE_ABI,PS3DK_PROF,SAMPLE_LIVE,path,(unsigned)SAMPLE_PHASE_SECONDS,(unsigned)SAMPLE_SESSION_SECONDS);
    scene_owned=1;
    if(scene_init()){puts("WORKLOAD_FAIL scene-init");goto done;}
    uint64_t idle=sample_now_us();
    while(sample_now_us()-idle<(uint64_t)SAMPLE_IDLE_SECONDS*1000000){
        int poll=scene_poll();if(poll<0)goto done;if(poll>0){rc=0;goto done;}
        if(scene_render(0,"No-input idle baseline",0,0,0,&gpu_begin,&gpu_end,&gpu_valid))goto done;
    }
#if SAMPLE_PROF_EXT
    /* EXT order (design r2): the group first (its worker count sizes the lanes), then init_ex, the owner thread,
     * the image and one lane per worker, then the workers start with the lane EAs; the GPU domain attaches on a
     * quiescent context.  A profiler failure after init only turns that extension off. */
    if(pool_create()){puts("WORKLOAD_FAIL no usable SPE pool");goto done;}
    pool_owned=1;
    printf("WORKLOAD_POOL workers=%u\n",pool_count());
    if(pe_rt_start(pool_count())){puts("WORKLOAD_FAIL profiler-init");goto done;}
    prof_owned=1;
    if(pool_start_ext()){puts("WORKLOAD_FAIL no usable SPE pool");pool_owned=0;goto done;}
    pe_rt_gpu_start();
    {
        char line[96];
        snprintf(line,sizeof line,"WORKLOAD_PROF_EXT spu=%d gpu=%d workers=%u\n",pe_rt_spu_on(),pe_rt_gpu()!=0,pool_count());
        ext_note(path,line);
    }
#else
    if(pool_init()){puts("WORKLOAD_FAIL no usable SPE pool");goto done;}
    pool_owned=1;
    printf("WORKLOAD_POOL workers=%u\n",pool_count());
#endif
#if PS3DK_PROF && !SAMPLE_PROF_EXT
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.app="heterogeneous";cfg.build_id=SAMPLE_BUILD_ID;cfg.workload_tag="ppu-spe-rsx-loop";
    cfg.mode=PS3DK_PROF_MODE_APP;cfg.max_threads=1;cfg.max_descriptors=64;cfg.ring_records=512;
    if(ps3dk_prof_set_sink_path("/dev_hdd0/tmp") || PS3DK_PROF_INIT(&cfg)){puts("WORKLOAD_FAIL profiler-init");goto done;}
    prof_owned=1;
    if(PS3DK_PROF_THREAD_BEGIN("main")){puts("WORKLOAD_FAIL profiler-thread");goto done;}
#endif
#if SAMPLE_LIVE
    {
        /* The live link serves this run's capture file; it never changes what is captured. */
        char status[96];
        sample_live_begin(path,status,sizeof status);
        scene_set_status(status);
    }
#endif
#if SAMPLE_PROF_EXT
    pe_rt_gpu_calibrate(); /* the start of the measured region */
    uint32_t keepalive_seq=0x20000000u; /* distinct from frame sequences, which stay below it */
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
#if SAMPLE_PROF_EXT
            pe_rt_gpu_calibrate(); /* every phase boundary */
#endif
            if(fflush(log)){puts("WORKLOAD_FAIL log-flush");goto done;}
        }
        PS3DK_PROF_FRAME();
#if SAMPLE_PROF_EXT
        pe_rt_gpu_frame();
        if(sequence>=0x20000000u-1u || keepalive_seq>=PE_SEQ_MASK-1u){puts("WORKLOAD_FAIL sequence-limit");goto done;}
#endif
        if(sequence==UINT32_MAX){puts("WORKLOAD_FAIL sequence-limit");goto done;}
        ++sequence;
        work_result result={0,0};uint64_t compute_begin=sample_now_us();
        if(phase==0 || phase==3){
            PS3DK_PROF_ZONE("compute.ppu_serial");
#if SAMPLE_PROF_EXT
            /* idle workers still get their epoch resets on time (prof_ext.h) */
            if(pool_keepalive(++keepalive_seq)){puts("WORKLOAD_FAIL keepalive");goto done;}
#endif
            result=work_partition(0,1,WORK_ITEMS);
        }else{
            {
                PS3DK_PROF_ZONE("compute.spe_dispatch");
#if SAMPLE_PROF_EXT
                if(pool_submit_ext(sequence)){puts("WORKLOAD_FAIL dispatch");goto done;}
#else
                if(pool_submit(sequence)){puts("WORKLOAD_FAIL dispatch");goto done;}
#endif
            }
            {
                PS3DK_PROF_ZONE("compute.spe_wait");
                while(!pool_ready(sequence)){
                    if(scene_poll()<0 || sample_now_us()-compute_begin>=2000000){
#if SAMPLE_PROF_EXT
                        pool_failed_ext("the SPE job deadline");
#endif
                        puts("WORKLOAD_FAIL SPE deadline");goto done;
                    }
                    usleep(100);
                }
            }
#if SAMPLE_PROF_EXT
            pool_done_ext();
#endif
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
#if SAMPLE_PROF_EXT
    {
        /* kept for the single END receipt, written after the CSV's checked finalization (below) */
        if(pool_owned && pool_close_ext(&ext_join)!=0)rc=1;
        if(prof_owned && pe_rt_stop(ext_join)!=0)rc=1;
        ext_eligible=pool_w32_eligible(&ext_why);
        ext_ran=1;
    }
#else
    if(pool_owned && pool_close()!=0)rc=1;
    if(prof_owned && PS3DK_PROF_SHUTDOWN()!=0)rc=1;
#endif
#if SAMPLE_LIVE
    sample_live_end(); /* after shutdown: a connected viewer receives END first */
#endif
    if(scene_owned)scene_close();
    fprintf(log,"END,%u,%u,0,0,%u,%d,0,0,0,0,0,0,0\n",frames,phase_seen,checked,rc);
    if(fflush(log) || fsync(fileno(log)))rc=1;
    if(fclose(log))rc=1;
    printf("WORKLOAD_END frames=%u checked=%u phase_mask=%u rc=%d\n",frames,checked,phase_seen,rc);
#if SAMPLE_PROF_EXT
    if(ext_ran){
        /* the whole acceptance result, once, with the FINAL rc (after the CSV's fflush/fsync/fclose) */
        char line[224];
        if(pe_end_line(line,sizeof line,ext_join,ext_eligible,ext_why,rc,frames,checked,phase_seen)<0 || ext_publish_end(path,line))
            puts("WORKLOAD_PROF_EXT END receipt NOT published: the run cannot be accepted");
    }
#endif
    return rc;
}
