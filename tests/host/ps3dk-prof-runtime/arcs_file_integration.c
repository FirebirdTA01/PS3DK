#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include "prof_arcs_file.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *mode;
static unsigned checks, output_closes, spool_closes;
static int output_fd=-1, spool_fd=-1, timing_fd=-1;
static struct ps3dk_prof_arc_file file;
static struct ps3dk_prof_arc_stream stream;
static struct ps3dk_prof_stream_slot slots[1];
static struct ps3dk_prof_stream_final finals[1];
static unsigned char scratch[31];
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static int is(const char *s) {return !strcmp(mode,s);}
static int io_open(void *ctx,const char *path,int rw,int *fd,int *err) {
    (void)ctx;int rc=rw?pal_open_rw_excl(path,fd,err):pal_open_excl(path,fd,err);
    if(!rc){if(rw)spool_fd=*fd;else output_fd=*fd;}return rc;
}
static long io_read(void *c,int fd,void *p,unsigned long n,int *e) {(void)c;return pal_read(fd,p,n,e);}
static long io_write(void *c,int fd,const void *p,unsigned long n,int *e) {(void)c;return pal_write(fd,p,n,e);}
static int io_rewind(void *c,int fd,int *e) {(void)c;return pal_rewind(fd,e);}
static int io_sync(void *c,int fd,int *e) {
    (void)c;if(fd==output_fd&&is("flush-fail")){*e=EIO;return -1;}return pal_sync(fd,e);
}
static int io_close(void *c,int fd,int *e) {
    (void)c;int rc=pal_close(fd,e);
    if(fd==output_fd)++output_closes;else {CHECK(fd==spool_fd);++spool_closes;}
    if((fd==output_fd&&is("output-close"))||(fd==spool_fd&&is("spool-close"))){*e=EIO;return -1;}
    return rc;
}
static const struct ps3dk_prof_file_ops ops={io_open,io_read,io_write,io_rewind,io_sync,io_close};
static long timing_write(void *c,const void *p,unsigned long n) {(void)c;int e=0;return pal_write(timing_fd,p,n,&e);}
static int timing_flush(void *c) {(void)c;int e=0;return pal_sync(timing_fd,&e);}
static int timing_close(void *c) {(void)c;int e=0;int fd=timing_fd;timing_fd=-1;return pal_close(fd,&e);}
static int consume(void *ctx,const struct ps3dk_prof_arc_view *v) {(void)ctx;return ps3dk_prof__stream_accept(&stream,v);}
static int finish(void *ctx,uint64_t n) {(void)ctx;return ps3dk_prof__stream_finish(&stream,n);}
static void *worker(void *arg) {
    (void)arg;int rc=ps3dk_prof_thread_begin("worker");
    if(is("unregistered")){CHECK(rc==PS3DK_PROF_EFULL);return NULL;}
    CHECK(rc==0);CHECK(ps3dk_prof__record_arc(4,8,0)==0);CHECK(ps3dk_prof__record_arc(4,8,0)==0);
    if(!is("unknown"))ps3dk_prof_thread_end();return NULL;
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;mode=argv[1];int err=0;
    CHECK(ps3dk_prof__file_init(&file,argv[2],"capture",&ops,NULL)==0);
    CHECK(ps3dk_prof__stream_init(&stream,slots,finals,1,4,1048576,ps3dk_prof__file_spool_write,&file)==0);
    char path[1024];CHECK(snprintf(path,sizeof path,"%s/capture.prof",argv[2])>0);
    CHECK(pal_open_excl(path,&timing_fd,&err)==0);
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.max_threads=1;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;
    cfg.app="file-adapter";cfg.build_id="build";cfg.flags="file-test";
    if(is("thread"))cfg.mode=PS3DK_PROF_MODE_THREAD;
    ps3dk_prof_sink_callbacks sink={timing_write,timing_flush,timing_close,NULL};
    struct ps3dk_prof_arc_options arcs={4,consume,finish,NULL};
    CHECK(ps3dk_prof_set_sink_callbacks(&sink)==0);CHECK(ps3dk_prof__configure_arcs(&arcs)==0);
    CHECK(ps3dk_prof_init(&cfg)==0);
    if(is("unregistered"))CHECK(ps3dk_prof_thread_begin("main")==0);
    for(unsigned i=0;i<(is("reuse")?4u:1u);++i) {
        pthread_t t;CHECK(pthread_create(&t,NULL,worker,NULL)==0);CHECK(pthread_join(t,NULL)==0);
        if(is("reuse"))CHECK(ps3dk_prof_drain()==0);
    }
    if(is("unregistered"))ps3dk_prof_thread_end();
    int shutdown=ps3dk_prof_shutdown();CHECK(shutdown==(is("unknown")?PS3DK_PROF_ENOTQUIESCED:0));
    if(is("corrupt")||is("truncated")||is("extra")) {
        int fd=open(file.spool_path,O_WRONLY);CHECK(fd>=0);unsigned char b=0xff;
        if(is("corrupt"))CHECK(pwrite(fd,&b,1,0)==1);
        if(is("truncated"))CHECK(ftruncate(fd,(off_t)stream.body_bytes-1)==0);
        if(is("extra"))CHECK(pwrite(fd,&b,1,(off_t)stream.body_bytes)==1);
        CHECK(close(fd)==0);
    }
    CHECK(ps3dk_prof__file_prepare(&file)==0);
    struct ps3dk_prof_arc_output output={ps3dk_prof__file_output_write,ps3dk_prof__file_output_flush,
                                      ps3dk_prof__file_output_close,&file};
    int rc=ps3dk_prof__stream_export(&stream,is("preflight")?0:1,ps3dk_prof__file_read,&file,
                                    &output,scratch,sizeof scratch);
    if(is("preflight")) {
        CHECK(rc==PS3DK_PROF_STREAM_INVALID&&!stream.export_attempted&&output_closes==0);
    } else {
        int io=is("corrupt")||is("truncated")||is("extra")||is("flush-fail")||is("output-close");
        CHECK(rc==(io?PS3DK_PROF_STREAM_IO:0));CHECK(output_closes==1);
    }
    int clean=ps3dk_prof__file_cleanup(&file);
    CHECK(clean==((is("flush-fail")||is("output-close")||is("spool-close"))?PS3DK_PROF_STREAM_IO:0));
    CHECK(output_closes==1&&spool_closes==1);
    CHECK(ps3dk_prof__file_cleanup(&file)==clean&&output_closes==1&&spool_closes==1);
    CHECK(access(file.pending_path,F_OK)==0&&access(file.spool_path,F_OK)==0);
    CHECK(snprintf(path,sizeof path,"%s/capture.arcs",argv[2])>0);CHECK(access(path,F_OK)<0&&errno==ENOENT);
    printf("checks=%u export_rc=%d cleanup_rc=%d published=0\n",checks,rc,clean);return 0;
}
