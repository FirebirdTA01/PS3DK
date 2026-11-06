/* Candidate-source filesystem measurement. No compiler hooks or automatic
 * startup, no publication, and no deletion of retained test artifacts. */
#include "prof_arcs_file.h"
#include "../pal/pal.h"
#include <ps3dk/prof.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned tests, passed;
static int timing_fd=-1;
static unsigned char pattern[65537], actual[65537], scratch[4096];
static struct ps3dk_prof_arc_file file;
static struct ps3dk_prof_arc_stream stream;
static struct ps3dk_prof_stream_slot slots[1];
static struct ps3dk_prof_stream_final finals[1];
static int worker_result;

static int check(const char *name,int ok,long rc,int err)
{
    ++tests;
    if(ok)++passed;
    printf("TEST %s %s rc=%ld errno=%d\n",name,ok?"PASS":"FAIL",rc,err);
    return ok;
}
static int close_fd(int *owned,int *err)
{
    if(*owned<0)return 0;
    int fd=*owned;*owned=-1;
    return pal_close(fd,err);
}
static int write_pattern(int fd,int *err)
{
    size_t off=0;
    while(off<sizeof pattern) {
        unsigned long n=(unsigned long)(sizeof pattern-off);
        if(n>65536)n=65536;
        long written=pal_write(fd,pattern+off,n,err);
        if(written<=0||(unsigned long)written>n)return -1;
        off+=(size_t)written;
    }
    return 0;
}
static int read_pattern(int fd,int *err)
{
    size_t off=0;
    while(off<sizeof actual) {
        unsigned long n=(unsigned long)(sizeof actual-off);
        if(n>65536)n=65536;
        long count=pal_read(fd,actual+off,n,err);
        if(count<=0||(unsigned long)count>n)return -1;
        off+=(size_t)count;
    }
    return memcmp(actual,pattern,sizeof pattern)==0?0:-1;
}
static int collision(const char *path)
{
    int extra=-1,err=0;
    int rc=pal_open_rw_excl(path,&extra,&err);
    int ok=rc<0&&err==EEXIST;
    if(extra>=0) {
        int close_err=0;
        close_fd(&extra,&close_err);
    }
    return check("rw-exclusive-collision",ok,rc,err);
}
static int filesystem_probe(const char *path)
{
    int fd=-1,err=0,rc;
    for(size_t i=0;i<sizeof pattern;++i)pattern[i]=(unsigned char)(i*17u);
    rc=pal_open_rw_excl(path,&fd,&err);
    if(!check("rw-exclusive-open",rc==0,rc,err))goto done;
    rc=write_pattern(fd,&err);
    if(!check("write-65537",rc==0,rc,err))goto done;
    rc=pal_sync(fd,&err);
    if(!check("sync",rc==0,rc,err))goto done;
    if(!collision(path))goto done;
    rc=pal_rewind(fd,&err);
    if(!check("same-fd-rewind",rc==0,rc,err))goto done;
    rc=read_pattern(fd,&err);
    if(!check("same-fd-read-and-sentinel",rc==0,rc,err))goto done;
    long n=pal_read(fd,actual,1,&err);
    if(!check("exact-eof",n==0,n,err))goto done;
    rc=close_fd(&fd,&err);
    if(!check("probe-close",rc==0,rc,err))goto done;
    if(!collision(path))goto done;
    /* Test-only readback of this probe's retained sentinel after the collision.
     * Arc replay below still uses the SAME original spool descriptor. */
    fd=open(path,O_RDONLY);
    if(!check("sentinel-open",fd>=0,fd,fd<0?errno:0))goto done;
    rc=read_pattern(fd,&err);
    if(!check("sentinel-after-closed-collision",rc==0,rc,err))goto done;
done:
    if(fd>=0) {
        rc=close_fd(&fd,&err);
        check("probe-cleanup-close",rc==0,rc,err);
    }
    return tests==passed?0:-1;
}
static long timing_write(void *ctx,const void *p,unsigned long n)
{
    (void)ctx;int err=0;return pal_write(timing_fd,p,n,&err);
}
static int timing_flush(void *ctx)
{
    (void)ctx;int err=0;return pal_sync(timing_fd,&err);
}
static int timing_close(void *ctx)
{
    (void)ctx;int err=0;return close_fd(&timing_fd,&err);
}
static int consume(void *ctx,const struct ps3dk_prof_arc_view *v)
{
    (void)ctx;return ps3dk_prof__stream_accept(&stream,v);
}
static int finish(void *ctx,uint64_t n)
{
    (void)ctx;return ps3dk_prof__stream_finish(&stream,n);
}
static void *worker(void *arg)
{
    (void)arg;
    worker_result=ps3dk_prof_thread_begin("file-worker");
    if(worker_result)return NULL;
    worker_result=ps3dk_prof__record_arc(4,8,0);
    if(!worker_result)worker_result=ps3dk_prof__record_arc(4,8,0);
    ps3dk_prof_thread_end();
    return NULL;
}
int main(void)
{
    char token[65],probe_path[1024],timing_path[1024],final_path[1024];
    int err=0,rc,started=0,cleaned=0;
    const char *dir=PROFILER_TEST_DIRECTORY;
    printf("PROF_FILES directory=%s abi=%s mode=%s candidate=1 published=0\n",
           dir,PROFILER_ABI_NAME,PROFILER_MODE_NAME);
    if(!check("pointer-width",sizeof(void *)==PROFILER_EXPECT_PTR_BYTES,(long)sizeof(void *),0))goto done;
    rc=snprintf(token,sizeof token,"prof-files-%s-%s-%016llx",PROFILER_ABI_NAME,
                PROFILER_MODE_NAME,(unsigned long long)pal_tb());
    if(!check("token",rc>0&&(size_t)rc<sizeof token,rc,0))goto done;
    int pn=snprintf(probe_path,sizeof probe_path,"%s/%s.probe",dir,token);
    int tn=snprintf(timing_path,sizeof timing_path,"%s/%s.prof",dir,token);
    int fn=snprintf(final_path,sizeof final_path,"%s/%s.arcs",dir,token);
    if(!check("paths",pn>0&&tn>0&&fn>0&&(size_t)pn<sizeof probe_path&&
              (size_t)tn<sizeof timing_path&&(size_t)fn<sizeof final_path,0,0))goto done;
    printf("PROBE_PATH %s\n",probe_path);
    if(filesystem_probe(probe_path))goto done;
    rc=ps3dk_prof__file_init(&file,dir,token,NULL,NULL);
    if(!check("adapter-init",rc==0,rc,file.os_error))goto done;
    rc=ps3dk_prof__stream_init(&stream,slots,finals,1,4,1048576,ps3dk_prof__file_spool_write,&file);
    if(!check("stream-init",rc==0,rc,0))goto done;
    rc=pal_open_excl(timing_path,&timing_fd,&err);
    if(!check("timing-open",rc==0,rc,err))goto done;
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.max_threads=1;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;
    cfg.app="file-probe";cfg.build_id="candidate-files";cfg.flags="manual-record-test";
    cfg.workload_tag=PROFILER_MODE_NAME;
#ifdef PROFILER_TEST_THREAD
    cfg.mode=PS3DK_PROF_MODE_THREAD;
    const unsigned workers=1;
#else
    const unsigned workers=4;
#endif
    ps3dk_prof_sink_callbacks sink={timing_write,timing_flush,timing_close,NULL};
    struct ps3dk_prof_arc_options arcs={4,consume,finish,NULL};
    rc=ps3dk_prof_set_sink_callbacks(&sink);
    if(!check("set-timing-sink",rc==0,rc,0))goto done;
    rc=ps3dk_prof__configure_arcs(&arcs);
    if(!check("configure-arcs",rc==0,rc,0))goto done;
    rc=ps3dk_prof_init(&cfg);
    if(!check("runtime-init",rc==0,rc,0))goto done;
    started=1;
    for(unsigned i=0;i<workers;++i) {
        pthread_t t;
        rc=pthread_create(&t,NULL,worker,NULL);
        if(!check("worker-create",rc==0,rc,0))goto done;
        rc=pthread_join(t,NULL);
        if(!check("worker-join",rc==0,rc,0))goto done;
        if(!check("record-two-arcs",worker_result==0,worker_result,0))goto done;
#ifndef PROFILER_TEST_THREAD
        rc=ps3dk_prof_drain();
        if(!check("app-drain-releases-slot",rc==0,rc,0))goto done;
#endif
    }
    rc=ps3dk_prof_shutdown();started=0;
    if(!check("runtime-shutdown",rc==0,rc,0))goto done;
    rc=ps3dk_prof__file_prepare(&file);
    if(!check("adapter-prepare",rc==0,rc,file.os_error))goto done;
    struct ps3dk_prof_arc_output output={ps3dk_prof__file_output_write,ps3dk_prof__file_output_flush,
                                      ps3dk_prof__file_output_close,&file};
    rc=ps3dk_prof__stream_export(&stream,1,ps3dk_prof__file_read,&file,&output,scratch,sizeof scratch);
    if(!check("export",rc==0,rc,file.os_error))goto done;
    rc=ps3dk_prof__file_cleanup(&file);
    cleaned=1;
    if(!check("adapter-cleanup",rc==0,rc,file.os_error))goto done;
    rc=access(final_path,F_OK);err=errno;
    if(!check("no-final-name",rc<0&&err==ENOENT,rc,err))goto done;
    printf("TIMING_PATH %s\nARCS_PATH %s\nSPOOL_PATH %s\nEXPECTED incarnations=%u count_per_edge=2 total=%u\n",
           timing_path,file.pending_path,file.spool_path,workers,workers*2);
done:
    if(started) {
        rc=ps3dk_prof_shutdown();check("failure-shutdown",rc==0,rc,0);
    }
    if(file.initialized&&!cleaned) {
        rc=ps3dk_prof__file_cleanup(&file);check("final-cleanup",rc==0,rc,file.os_error);
    }
    if(timing_fd>=0) {
        rc=close_fd(&timing_fd,&err);check("timing-cleanup-close",rc==0,rc,err);
    }
    printf("PROF_FILES_DONE tests=%u pass=%u fail=%u\n",tests,passed,tests-passed);
    return tests==passed?0:1;
}
